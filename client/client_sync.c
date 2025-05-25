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


void inotify_cleanup_handler(void *arg) {
    int fd = *(int*)arg;
    if (fd >= 0) { 
        printf("\n[Inotify Thread] Cleanup: Fechando inotify_fd %d\n", fd);
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
        fprintf(stderr, "\n[Cliente Sync] Erro na abertura do arquivo '%s' para download (server-initiated).\n", path);
        fflush(stderr);
        return;
    }
    printf("\n[Cliente Sync] Servidor iniciou atualização para: %s. Salvando em: %s\n", filename, path);
    fflush(stdout);

    packet_t pkt;
    int error_occurred = 0;
    int download_completed_flag = 0;
    pkt.payload_size = 1; // Inicializa para entrar no loop


    while (pkt.payload_size != 0) { 
        pthread_mutex_lock(&socket_mutex);
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
        pthread_mutex_unlock(&socket_mutex);
        if (error_occurred || pkt.payload_size == 0) break;
    }
    fclose(f);
    if (download_completed_flag && !error_occurred) {
        printf("\n[Cliente Sync] Arquivo '%s' atualizado com sucesso via servidor.\n", filename);
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
    int sock = *(int*)parameter; 
    char sync_dir_abs_path[PATH_MAX];
    if (!getcwd(sync_dir_abs_path, sizeof(sync_dir_abs_path))) {
        perror("\n[Inotify Thread] Erro ao obter CWD para inotify");
        pthread_exit(NULL);
    }
    printf("\n[Inotify Thread] Monitorando diretório: %s para mudanças...\n", sync_dir_abs_path);
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
        // pthread_cleanup_pop(1); // <<--- REMOVIDO DAQUI
        // inotify_fd = -1;       // <<--- REMOVIDO DAQUI (handler deve ser robusto ou fd não é alterado antes do pop)
        pthread_exit(NULL); // pthread_exit executará os handlers de cleanup automaticamente
    }

    // Loop principal para monitorar eventos do inotify
    while (1) { 
        pthread_testcancel(); 
        int n = read(inotify_fd, buf, INOTIFY_BUF_LEN); 
        if (n <= 0) {
            if (n < 0) {
                if (errno == EINTR) continue; // Se interrompido por sinal, tenta novamente
                 // perror("\n[Inotify Thread] Erro na leitura do inotify_fd"); // Pode ser muito verboso se cancelado
            }
            // Se n == 0 (EOF no inotify_fd, improvável) ou erro não recuperável
            // fprintf(stderr, "\n[Inotify Thread] Leitura do inotify_fd retornou %d. Encerrando thread.\n", n);
            break; // Sai do loop while(1)
        }
        char* p = buf;
        while (p < buf + n) { // Loop interno para processar múltiplos eventos lidos de uma vez
            pthread_testcancel(); 
            struct inotify_event *event = (struct inotify_event*)p;
            if (event->len > 0 && event->name[0] != '.') { // Se tem nome e não é arquivo oculto
                char full_path[PATH_MAX];
                snprintf(full_path, PATH_MAX, "%s/%s", sync_dir_abs_path, event->name);
                if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO || event->mask & IN_CLOSE_WRITE) {
                    printf("\n[Inotify Thread] Evento: Arquivo '%s' criado/modificado. Enviando...\n", event->name); fflush(stdout);
                    char *upload_msg = upload_file_action(full_path, sock); 
                    if (upload_msg) {
                        if (strcmp(upload_msg, "Arquivo enviado com sucesso.") != 0) {
                           fprintf(stderr, "[Inotify Thread] Upload: %s\n", upload_msg); fflush(stderr); free(upload_msg);
                        } else { printf("[Inotify Thread] Upload: %s\n", upload_msg); fflush(stdout); }
                    }
                } else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                    printf("\n[Inotify Thread] Evento: Arquivo '%s' deletado. Solicitando deleção...\n", event->name); fflush(stdout);
                    char *delete_msg = delete_file_action(event->name, sock); 
                     if (delete_msg) { printf("[Inotify Thread] Delete: %s\n", delete_msg); fflush(stdout); free(delete_msg); }
                }
            }
            p += sizeof(struct inotify_event) + event->len;
        } // Fim do loop interno (p < buf + n)
    } // Fim do loop while(1) principal

    pthread_cleanup_pop(1); // Remove e EXECUTA o handler de cleanup (para fechar inotify_fd)
    
    printf("\n[Inotify Thread] Encerrando normalmente...\n"); fflush(stdout);
    pthread_exit(NULL);
} // Fim da função notify_file_change_thread

void *server_updates_listener_thread(void *arg) {
    int sock = *(int *)arg;
    char sync_dir_effective_path[PATH_MAX];
    if (!getcwd(sync_dir_effective_path, sizeof(sync_dir_effective_path))) {
        perror("\n[Listener Thread] Falha ao obter CWD");
        pthread_exit(NULL);
    }
    printf("\n[Listener Thread] Escutando atualizações do servidor...\n"); fflush(stdout);
    
    fd_set read_fds;
    struct timeval tv;
    packet_t pkt;

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
            int recv_status = recv_packet(sock, &pkt);
            pthread_mutex_unlock(&socket_mutex); 

            if (recv_status != 0) {
                break; 
            }
            
            char fn[MAX_PAYLOAD+1];
            if (pkt.payload_size > 0 && pkt.payload_size <= MAX_PAYLOAD) {
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
                fn[0] = '\0';
            }
            
            if(pkt.type == PKT_UPLOAD_REQ){       
                printf("\n[Listener Thread] Servidor requisitou UPLOAD para arquivo '%s' (propagação).\n", fn); fflush(stdout);
                packet_t r_ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };    
                
                pthread_mutex_lock(&socket_mutex);         
                int ack_sent_ok = (send_packet(sock, &r_ack) == 0);
                pthread_mutex_unlock(&socket_mutex);

                if(!ack_sent_ok) {
                    fprintf(stderr, "\n[Listener Thread] Falha ao enviar ACK para UPLOAD_REQ do servidor para '%s'.\n", fn); fflush(stderr);
                    continue; 
                }
                handle_server_initiated_download(sock, fn, sync_dir_effective_path); 
            } else if (pkt.type == PKT_DELETE_REQ) {
                printf("\n[Listener Thread] Servidor requisitou DELETE para arquivo '%s'.\n", fn); fflush(stdout);
                packet_t r_ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };

                pthread_mutex_lock(&socket_mutex);
                int ack_sent_ok = (send_packet(sock, &r_ack) == 0);
                pthread_mutex_unlock(&socket_mutex);

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
    printf("\n[Listener Thread] Thread de escuta terminando.\n"); fflush(stdout);
    pthread_exit(NULL);
}
