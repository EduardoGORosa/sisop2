#include "client_sync.h"
#include "client_actions.h" 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     
#include <dirent.h>     
#include <errno.h>      
#include <pthread.h>
#include <sys/select.h> 
#include <sys/time.h>   

typedef struct {
    int sock;
    pthread_mutex_t *socket_mutex;
} ThreadArgs;


#define MAX_FILE_NAME_LEN 256
#define MAX_NUM_IGNORE_EVENTS 256
char ignore_event[MAX_NUM_IGNORE_EVENTS][MAX_FILE_NAME_LEN];
int ignore_event_pointer = 0;

void add_ignore_file(const char *file_name) {
    printf("Arquivo %s sendo adicionado ao ignore\n", file_name );
    if (strlen(file_name) > MAX_FILE_NAME_LEN) {
        printf("Nome do arquivo ultrapassa o limite de caracteres, erros irão acontecer.\n");    // todo: tornar o tamanho do nome do arquivo dinamico
        return;
    } 
    if (ignore_event_pointer >= MAX_NUM_IGNORE_EVENTS-1 ){
        ignore_event_pointer = 0;
    }
    printf("Executando agora o strcpy na posição %s usando o nome de arquivo %s\n",ignore_event[ignore_event_pointer + 1],file_name);
    strcpy(ignore_event[ignore_event_pointer + 1], file_name);
    ignore_event_pointer++;
    printf("Arquivo %s inserido na lista de ignore. pointer da lista: %i\n", file_name, ignore_event_pointer );
}

int event_should_be_ignored(const char *file_name){
    for (int i = 0; i < MAX_NUM_IGNORE_EVENTS; i++) {
        if(ignore_event[i] != NULL && ignore_event[i] != ""){
            if (strcmp(ignore_event[i], file_name) == 0) {
                printf("Arquivo %s estava na lista de ignore e foi removido.\n", file_name);
                strcpy(ignore_event[i], "");
                return 1;
            }
        }
    }
    printf("Arquivo %s não estava na lista de ignore.\n", file_name);
    return 0;
}

void inotify_cleanup_handler(void *arg) {
    int fd = *(int*)arg;
    if (fd >= 0) { 
        fflush(stdout);
        close(fd);
        // *(int*)arg = -1; // Opcional: invalida o fd na variável original se o ponteiro for para ela
    }
}

void handle_server_initiated_download(int sock, const char *filename, const char* sync_dir_abs_path) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", sync_dir_abs_path, filename);
    
    FILE *f = fopen(path, "wb");
    if (!f) {
        fflush(stderr);
        return;
    }
    fflush(stdout);

    packet_t pkt;
    int error_occurred = 0;
    int download_completed_flag = 0;
    pkt.payload_size = 1; // Inicializa para entrar no loop


    while (pkt.payload_size != 0) { 
        if (recv_packet(sock, &pkt) != 0) {
            error_occurred = 1;
        } else {
            if (pkt.type != PKT_UPLOAD_DATA && pkt.type != PKT_DOWNLOAD_DATA) { 
                fprintf(stderr, "\n[Cliente Sync] Recebido tipo de pacote inesperado (%d) para '%s'.\n", pkt.type, filename);
                fflush(stderr);
                error_occurred = 1;
            } else {
                packet_t ca = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
                if (send_packet(sock, &ca) != 0) {
                    error_occurred = 1;
                } else {
                    if (pkt.payload_size == 0) { 
                        download_completed_flag = 1;
                    } else {                                                
                        if (fwrite(pkt.payload, 1, pkt.payload_size, f) != pkt.payload_size) {
                            fprintf(stderr, "\n[Cliente Sync] Erro ao escrever no arquivo '%s'.\n", path);
                            fflush(stderr);
                            error_occurred = 1;
                        }
                    }
                }
            }
        }
        if (error_occurred || pkt.payload_size == 0) break;
    }
    fclose(f);
    if (download_completed_flag && !error_occurred) {
        fflush(stdout);
    } else {
        if (error_occurred && pkt.payload_size != 0) { 
             printf("\n[Cliente Sync] Download do arquivo '%s' via servidor falhou ou incompleto.\n", filename);
             fflush(stdout);
        }
        // Somente remove se o download não foi completado E não houve outro erro E o arquivo existe
        if(!download_completed_flag && !error_occurred && access(path, F_OK) == 0 ) { 
            remove(path); 
        }
    }
}

void *notify_file_change_thread(void *parameter) {
    ThreadArgs *args = (ThreadArgs *)parameter;
    int sock = args->sock;
    pthread_mutex_t *mutex = args->socket_mutex;

    char sync_dir_abs_path[PATH_MAX];
    if (!getcwd(sync_dir_abs_path, sizeof(sync_dir_abs_path))) {
        perror("\n[Inotify Thread] Erro ao obter CWD para inotify");
        pthread_exit(NULL);
    }
    fflush(stdout);

    char buf[INOTIFY_BUF_LEN]; 
    int inotify_fd = inotify_init(); // Inicializa fd
    if (inotify_fd < 0) { 
        perror("[Inotify Thread] inotify_init"); 
        pthread_exit(NULL); 
    }

    pthread_cleanup_push(inotify_cleanup_handler, &inotify_fd); // Registra handler de cleanup

    int wd = inotify_add_watch(inotify_fd, sync_dir_abs_path, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM);
    if (wd < 0) { 
        perror("[Inotify Thread] inotify_add_watch"); 
        pthread_exit(NULL); // pthread_exit executará os handlers de cleanup automaticamente
    }

    // Loop principal para monitorar eventos do inotify
    while (1) { 
        pthread_testcancel();         
        int n = read(inotify_fd, buf, INOTIFY_BUF_LEN); 
        if (n <= 0) {
            if (n < 0) {
                if (errno == EINTR) continue; // Se interrompido por sinal, tenta novamente
            }
            break; // Sai do loop while(1)
        }
        char* p = buf;
        while (p < buf + n) { // Loop interno para processar múltiplos eventos lidos de uma vez            
            pthread_testcancel(); 
            pthread_mutex_lock(mutex);         
            printf("Entrou no lock 9\n");
            struct inotify_event *event = (struct inotify_event*)p;
            printf("Evento identificado. Nome: %s . tamamnho: %i\n",event->name , event->len);               
            if(event_should_be_ignored(event->name)){
                break;
            }        
            if (event->len > 0 && event->name[0] != '.') { // Se tem nome e não é arquivo oculto
                char full_path[PATH_MAX];
                snprintf(full_path, PATH_MAX, "%s/%s", sync_dir_abs_path, event->name);
                if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO || event->mask & IN_CLOSE_WRITE) {
                    printf("\n[Inotify Thread] Evento: Arquivo '%s' criado/modificado. Enviando...\n", event->name); fflush(stdout);
                    char *upload_msg = upload_file_action(full_path, sock); 
                    if (upload_msg) {
                        if (strcmp(upload_msg, "Arquivo enviado com sucesso.") != 0) {
                            fflush(stderr); free(upload_msg);
                        } else { fflush(stdout); }
                    }
                } else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    char *delete_msg = delete_file_action(event->name, sock); 
                     if (delete_msg) {  fflush(stdout); free(delete_msg); }
                }
            }
            p += sizeof(struct inotify_event) + event->len;
            pthread_mutex_unlock(mutex);         
            printf("Saiu no lock 9\n");
        } // Fim do loop interno (p < buf + n)
    } // Fim do loop while(1) principal

    free(args);
    pthread_cleanup_pop(1); // Remove e EXECUTA o handler de cleanup (para fechar inotify_fd)
    pthread_exit(NULL);
} // Fim da função notify_file_change_thread

void *server_updates_listener_thread(void *arg) {
    int sock = *(int *)arg;
    char sync_dir_effective_path[PATH_MAX];
    if (!getcwd(sync_dir_effective_path, sizeof(sync_dir_effective_path))) {
        perror("\n[Listener Thread] Falha ao obter CWD");
        pthread_exit(NULL);
    }
    
    fd_set read_fds;
    struct timeval tv;
    packet_t pkt;
    printf("Listener está funcionando\n");
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);

        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms timeout

        pthread_testcancel();
        int activity = select(sock + 1, &read_fds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) continue; 
            break; 
        }

        if (activity > 0 && FD_ISSET(sock, &read_fds)) {
            pthread_mutex_lock(&socket_mutex);
            printf("Entrou no lock 10\n");
            int recv_status = recv_packet(sock, &pkt);
            pthread_mutex_unlock(&socket_mutex); 
            printf("Saiu no lock 10\n");

            if (recv_status != 0) {
                printf("Listener saiu com status de erro \n");
                break; 
            }
            
            char fn[MAX_PAYLOAD+1];
            if (pkt.payload_size > 0 && pkt.payload_size <= MAX_PAYLOAD) {
                printf("Payload no listener está correto \n");
                memcpy(fn, pkt.payload, pkt.payload_size);
                size_t actual_len = pkt.payload_size;
                 if ( (actual_len > 0 && pkt.payload[actual_len-1] == '\0') || actual_len == 0) {
                    fn[actual_len > 0 ? actual_len-1 : 0] = '\0'; 
                } else if (actual_len < MAX_PAYLOAD) {
                    fn[actual_len] = '\0'; 
                } else { 
                    fn[MAX_PAYLOAD-1] = '\0'; 
                }
            } else {
                printf("Payload no listener está errado \n");
                fn[0] = '\0';
            }
            
            if(pkt.type == PKT_UPLOAD_REQ){       
                printf("\n[Listener Thread] Servidor requisitou UPLOAD para arquivo '%s' (propagação).\n", fn); fflush(stdout);
                packet_t r_ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };    
                
                pthread_mutex_lock(&socket_mutex);         
                printf("Entrou no lock 11\n");
                int ack_sent_ok = (send_packet(sock, &r_ack) == 0);
                
                if(!ack_sent_ok) {
                    fprintf(stderr, "\n[Listener Thread] Falha ao enviar ACK para UPLOAD_REQ do servidor para '%s'.\n", fn); fflush(stderr);
                    continue; 
                }
                handle_server_initiated_download(sock, fn, sync_dir_effective_path); 
                add_ignore_file(fn);
                pthread_mutex_unlock(&socket_mutex);
                printf("Saiu no lock 11\n");
            } else if (pkt.type == PKT_DELETE_REQ) {
                printf("\n[Listener Thread] Servidor requisitou DELETE para arquivo '%s'.\n", fn); fflush(stdout);
                packet_t r_ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };

                pthread_mutex_lock(&socket_mutex);
                printf("Entrou no lock 12\n");
                int ack_sent_ok = (send_packet(sock, &r_ack) == 0);
                pthread_mutex_unlock(&socket_mutex);
                printf("Saiu no lock 12\n");

                if(!ack_sent_ok) {
                     fprintf(stderr, "\n[Listener Thread] Falha ao enviar ACK para DELETE_REQ do servidor para '%s'.\n", fn); fflush(stderr);
                     continue; 
                }
                char local_file_to_delete[PATH_MAX];
                snprintf(local_file_to_delete, PATH_MAX, "%s/%s", sync_dir_effective_path, fn);
                if (remove(local_file_to_delete) == 0) {
                   printf("\n[Listener Thread] Arquivo '%s' deletado localmente por instrução do servidor.\n", local_file_to_delete); fflush(stdout);
                } else {
                   fprintf(stderr, "\n[Listener Thread] Erro ao deletar '%s' localmente: %s\n", local_file_to_delete, strerror(errno)); fflush(stderr);
                }
            } else if (pkt.type == PKT_SYNC_EVENT) {
                printf("\n[Listener Thread] Recebido PKT_SYNC_EVENT, ignorando.\n"); fflush(stdout);
            } else {
                printf("\n[Listener Thread] Aviso: Recebido pacote tipo %d (seq: %u). Não é uma ação de servidor para este listener.\n", pkt.type, pkt.seq_num);
                fflush(stdout);
            }
        }
        pthread_testcancel(); 
    }
    pthread_exit(NULL);
}
