// client/client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     
#include <limits.h>     
#include <arpa/inet.h>  
#include <pthread.h>    
#include <sys/stat.h>   
#include <sys/socket.h> 
#include <netdb.h>      
#include <errno.h>

#include "../common/packet.h"
#include "client_actions.h" 
#include "client_sync.h"    

char initial_cwd[PATH_MAX];
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;


// Função de cleanup em caso de falha após a criação do diretório sync_dir
// Declarada antes de main para que main possa usá-la.
void cleanup_sync_dir_and_exit(int sock, const char* p_initial_cwd, const char* p_sync_dir_path, int status) {
    if (sock != -1) close(sock);
    if (p_initial_cwd && p_sync_dir_path && strcmp(p_initial_cwd, "") != 0) {
        if (chdir(p_initial_cwd) == 0) {
             remove_directory_recursively(p_sync_dir_path);
        } else {
            fprintf(stderr, "Erro crítico: Não foi possível retornar ao diretório inicial para limpar %s.\n", p_sync_dir_path);
        }
    }
    exit(status);
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <user> <host> <port>\n", argv[0]);
        return 1;
    }
    const char *user = argv[1];
    const char *host = argv[2];
    const char *port_str = argv[3];

    initial_cwd[0] = '\0'; // Inicializa para o caso de getcwd falhar
    if (!getcwd(initial_cwd, sizeof(initial_cwd))) {
        perror("Erro ao obter diretório de trabalho inicial");
        return 1; // Não há sync_dir para limpar aqui ainda
    }

    char sync_dir_path[PATH_MAX];
    snprintf(sync_dir_path, PATH_MAX, "%s/sync_dir_%s", initial_cwd, user);

    printf("Limpando diretório de sincronização anterior (se existir): %s\n", sync_dir_path);
    fflush(stdout);
    remove_directory_recursively(sync_dir_path); 

    if (mkdir(sync_dir_path, 0755) != 0) {
        if (errno != EEXIST) { 
            perror("Erro ao criar o novo diretório sync_dir local");
            return 1; // Não há sync_dir para limpar aqui, pois mkdir falhou
        }
        printf("Diretório '%s' já existia (limpeza anterior pode ter falhado parcialmente).\n", sync_dir_path);
    } else {
        printf("Novo diretório de sincronização '%s' criado.\n", sync_dir_path);
    }
    
    if (chdir(sync_dir_path) != 0) {
        perror("Erro ao mudar para o diretório sync_dir local");
        cleanup_sync_dir_and_exit(-1, initial_cwd, sync_dir_path, 1); // Passa -1 para sock pois não foi aberto
    }
    printf("Diretório de trabalho atual: %s\n", sync_dir_path);


    struct addrinfo hints, *servinfo, *p_servaddr;
    int rv_getaddr;
    int sock = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv_getaddr = getaddrinfo(host, port_str, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv_getaddr));
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }

    for(p_servaddr = servinfo; p_servaddr != NULL; p_servaddr = p_servaddr->ai_next) {
        if ((sock = socket(p_servaddr->ai_family, p_servaddr->ai_socktype, p_servaddr->ai_protocol)) == -1) {
            continue;
        }
        if (connect(sock, p_servaddr->ai_addr, p_servaddr->ai_addrlen) == -1) {
            close(sock);
            sock = -1;
            continue;
        }
        break;
    }

    if (p_servaddr == NULL || sock == -1) {
        fprintf(stderr, "Cliente: falha ao conectar a %s:%s após tentar todos os endereços.\n", host, port_str);
        freeaddrinfo(servinfo);
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 2);
    }
    freeaddrinfo(servinfo);

    packet_t init_pkt = { .type = PKT_GET_SYNC_DIR, .seq_num = 1 };
    strncpy(init_pkt.payload, user, MAX_PAYLOAD - 1);
    init_pkt.payload[MAX_PAYLOAD - 1] = '\0';
    init_pkt.payload_size = (uint32_t)strlen(init_pkt.payload) + 1;

    pthread_mutex_lock(&socket_mutex);
    int init_send_ok = (send_packet(sock, &init_pkt) == 0);
    packet_t ack_pkt;
    int init_recv_ok = 0;
    if (init_send_ok) {
        init_recv_ok = (recv_packet(sock, &ack_pkt) == 0);
    }
    pthread_mutex_unlock(&socket_mutex);

    if (!init_send_ok) {
        fprintf(stderr, "Erro ao enviar informações de usuário para o servidor.\n");
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }
    if (!init_recv_ok || (ack_pkt.type != PKT_ACK && ack_pkt.type != PKT_NACK)) {
        fprintf(stderr, "Erro ao receber confirmação do servidor ou resposta inesperada.\n");
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }

    if (ack_pkt.type == PKT_NACK) {
        fprintf(stderr, "Servidor recusou a conexão (PKT_NACK). Motivo: %s\n", ack_pkt.payload_size > 0 ? ack_pkt.payload : "Não especificado");
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }
    printf("Conectado ao servidor como '%s'.\n", user);
    fflush(stdout);

    printf("Iniciando sincronização inicial com o servidor...\n");
    fflush(stdout);
    if (perform_initial_sync(sock) != 0) { 
        fprintf(stderr, "Falha durante a sincronização inicial. Alguns arquivos podem estar desatualizados.\n");
        fflush(stderr);
    } else {
        printf("Sincronização inicial concluída.\n");
        fflush(stdout);
    }

    pthread_t inotify_tid = 0; 
    pthread_t listener_tid = 0;
    int *sock_ptr_inotify = malloc(sizeof(int));
    int *sock_ptr_listener = malloc(sizeof(int));

    if (!sock_ptr_inotify || !sock_ptr_listener) {
        perror("malloc for thread args failed");
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }
    *sock_ptr_inotify = sock;
    *sock_ptr_listener = sock;

    if (pthread_create(&listener_tid, NULL, server_updates_listener_thread, sock_ptr_listener) != 0) {
        perror("pthread_create for listener failed");
        free(sock_ptr_inotify); free(sock_ptr_listener); 
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }
    if (pthread_create(&inotify_tid, NULL, notify_file_change_thread, sock_ptr_inotify) != 0) {
        perror("pthread_create for inotify failed");
        if (listener_tid != 0) { 
             pthread_cancel(listener_tid);
             pthread_join(listener_tid, NULL);
        }
        free(sock_ptr_inotify); free(sock_ptr_listener); 
        cleanup_sync_dir_and_exit(sock, initial_cwd, sync_dir_path, 1);
    }
    
    printf("Sessão iniciada para '%s'. Diretório de sincronização: '%s'\n", user, sync_dir_path);
    printf("Monitoramento de arquivos e escuta de atualizações do servidor ativos.\n");
    printf("Comandos:\n"
           " upload <caminho/absoluto/ou/relativo/ao/inicial/arquivo.ext>\n"
           " download <nome_arquivo.ext>\n"
           " delete <nome_arquivo.ext>\n"
           " list_server\n"
           " list_client\n"
           " exit\n");

    char line[512];
    // Loop de comando reescrito para clareza
    while (1) {
        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\nEOF ou erro de entrada detectado. Saindo...\n");
            break; // Trata Ctrl+D ou erro de leitura como 'exit'
        }

        char *cmd = strtok(line, " \t\n");
        if (!cmd) {
            continue;
        }

        char *arg = strtok(NULL, "\n");
        if (arg) {
            while (*arg == ' ' || *arg == '\t') arg++;
            char *end_arg = arg + strlen(arg) - 1;
            while (end_arg > arg && (*end_arg == ' ' || *end_arg == '\t')) *end_arg-- = '\0';
            if (strlen(arg) == 0) arg = NULL; // Considera argumento vazio como NULL após trim
        }

        if (strcmp(cmd, "upload") == 0) {
            if (arg) { // Verifica se arg não é NULL e não é vazio (já tratado acima)
                char *upload_msg = upload_file_action(arg, sock); 
                if (upload_msg) {
                    printf("%s\n", upload_msg);
                    if (strcmp(upload_msg, "Arquivo enviado com sucesso.") != 0) free(upload_msg);
                }
            } else printf("Uso: upload <caminho/do/arquivo.ext>\n");
        } else if (strcmp(cmd, "download") == 0) {
            if (arg) download_file_action(arg, sock, initial_cwd); 
            else printf("Uso: download <nome_arquivo.ext>\n");
        } else if (strcmp(cmd, "delete") == 0) {
            if (arg) {
                char* delete_msg = delete_file_action(arg, sock); 
                if (delete_msg) { printf("%s\n", delete_msg); free(delete_msg); }
            } else printf("Uso: delete <nome_arquivo.ext>\n");
        } else if (strcmp(cmd, "list_server") == 0) {
            list_server_files_action(sock); 
        } else if (strcmp(cmd, "list_client") == 0) {
            list_client_files_action(); 
        } else if (strcmp(cmd, "exit") == 0) {
            break; 
        } else {
            printf("Comando desconhecido: '%s'\n", cmd);
        }
        fflush(stdout); 
    } // Fim do while(1) para o loop de comandos

    printf("\nEncerrando cliente...\n");
    fflush(stdout);
    
    if (sock != -1) {
        printf("Desligando o socket...\n"); fflush(stdout);
        if (shutdown(sock, SHUT_RDWR) != 0) {
            //perror("Falha ao desligar o socket"); 
        }
    }

    // É importante que o listener_tid seja encerrado antes de tentar fechar o socket,
    // ou que o close(sock) sinalize o listener para terminar.
    // A ordem atual é shutdown, close, depois join.
    if (listener_tid != 0) { 
        printf("Aguardando encerramento do listener thread (pode levar um momento se o socket não fechou imediatamente o select)...\n"); fflush(stdout);
        // O close(sock) deve fazer o select/recv_packet no listener retornar erro.
        // Se o join travar aqui, o listener não está saindo do seu loop.
        if (pthread_join(listener_tid, NULL) != 0) {
             //perror("Falha ao fazer join no listener_thread após close/shutdown");
        } else {
            printf("Listener thread encerrado.\n"); fflush(stdout);
        }
    }
    // O socket já deve estar fechado aqui, o que pode impactar o sock_ptr_inotify se ele o usar.
    // O inotify_thread é cancelado, e suas ações de socket (upload/delete) falharão se tentadas
    // após o sock ser fechado.
    if (inotify_tid != 0) { 
        printf("Cancelando inotify thread...\n"); fflush(stdout);
        if (pthread_cancel(inotify_tid) != 0) {
            // perror("Falha ao cancelar inotify_thread");
        }
        printf("Aguardando encerramento do inotify thread...\n"); fflush(stdout);
        if (pthread_join(inotify_tid, NULL) != 0) {
            // perror("Falha ao fazer join no inotify_thread");
        } else {
            printf("Inotify thread encerrado.\n"); fflush(stdout);
        }
    }
    // Fechar o socket ANTES de fazer join nos threads que dependem dele (listener)
    // e cancelar o thread que não depende (inotify) é uma estratégia.
    // O inotify_fd é fechado pelo seu cleanup handler.
    if (sock != -1) { // Socket já deveria estar fechado, mas por segurança
        close(sock);
    }
        
    free(sock_ptr_inotify);
    sock_ptr_inotify = NULL;
    free(sock_ptr_listener);
    sock_ptr_listener = NULL;

    pthread_mutex_destroy(&socket_mutex); 

    printf("Limpando diretório de sincronização: %s\n", sync_dir_path);
    fflush(stdout);
    if (chdir(initial_cwd) != 0) { 
        perror("Erro crítico: Não foi possível retornar para o diretório inicial antes de limpar sync_dir");
    } else {
        if (remove_directory_recursively(sync_dir_path) != 0) {
            fprintf(stderr, "Aviso: Falha ao limpar completamente o diretório %s na saída.\n", sync_dir_path);
        } else {
            printf("Diretório de sincronização %s limpo com sucesso.\n", sync_dir_path);
        }
    }

    printf("Cliente encerrado.\n");
    return 0;
}
