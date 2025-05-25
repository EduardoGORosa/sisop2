// server/server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include "../common/packet.h"

#define SERVER_PORT   12345
#define BACKLOG       10
#define CHUNK_SIZE    MAX_PAYLOAD
#define MAX_USER_LEN  256
#define MAX_CONNECTIONS 2   // Valor especificado no enunciado do trabalho

typedef struct UserSess {
    char username[MAX_USER_LEN];
    int  count;
    int  conn_fd[MAX_CONNECTIONS];
    struct UserSess *next;
} UserSess;

static UserSess *sessions_head = NULL;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

// mkdir -p equivalent
static void mkdir_p(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

static UserSess *get_user_sess(const char *user) {
    for (UserSess *u = sessions_head; u; u = u->next) {
        if (strcmp(u->username, user) == 0) return u;
    }
    UserSess *u = calloc(1, sizeof(*u));
    strncpy(u->username, user, MAX_USER_LEN - 1);
    u->count = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        u->conn_fd[i] = 0;
    }
    u->next = sessions_head;
    sessions_head = u;
    return u;
}

int send_and_wait_ack(int s, packet_t *p) {
    send_packet(s, p);
    packet_t a;
    return (recv_packet(s, &a) == 0 && a.type == PKT_ACK) ? 0 : -1;
}

void *handle_client(void *arg) {
    int conn = *(int*)arg;
    free(arg);
    packet_t pkt;

    if (recv_packet(conn, &pkt) < 0 || pkt.type != PKT_GET_SYNC_DIR) {
        close(conn);
        return NULL;
    }

    char user[MAX_USER_LEN];
    size_t ulen = pkt.payload_size < (MAX_USER_LEN - 1)
                   ? pkt.payload_size
                   : (MAX_USER_LEN - 1);
    memcpy(user, pkt.payload, ulen);
    user[ulen] = '\0';

    pthread_mutex_lock(&sessions_mutex);
    UserSess *us = get_user_sess(user);
    if (us->count >= 2) {
        pthread_mutex_unlock(&sessions_mutex);
        packet_t nack = { .type = PKT_NACK, .seq_num = pkt.seq_num, .payload_size = 0 };
        send_packet(conn, &nack);
        close(conn);
        return NULL;
    }
    us->count++;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (us->conn_fd[i] == 0) {
            us->conn_fd[i] = conn;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);

    packet_t ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
    send_packet(conn, &ack);
    printf("[+] Sessão iniciada para '%s' (fd=%d) total=%d\n",
           user, conn, us->count);

    char base[512];
    snprintf(base, sizeof(base), "storage/%s/sync_dir", user);
    mkdir_p("storage", 0755);
    mkdir_p(base, 0755);

    while (recv_packet(conn, &pkt) == 0) {        
        if (pkt.type == PKT_SYNC_EVENT) {
            continue;
        }

        size_t flen = pkt.payload_size < CHUNK_SIZE ? pkt.payload_size : CHUNK_SIZE;
        char fn[CHUNK_SIZE+1];
        memcpy(fn, pkt.payload, flen);
        fn[flen] = '\0';

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", base, fn);

        switch (pkt.type) {
            case PKT_UPLOAD_REQ: {
                printf("[*] Upload: %s\n", fn);
                FILE *f = fopen(path, "wb");
                packet_t r = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };                
                send_packet(conn, &r);

                while (recv_packet(conn, &pkt) == 0 && pkt.payload_size > 0) {
                    fwrite(pkt.payload, 1, pkt.payload_size, f);
                    packet_t ca = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
                    send_packet(conn, &ca);
                }
                
                pthread_mutex_lock(&sessions_mutex);
                for (int i = 0; i < MAX_CONNECTIONS; i++) {
                    int other_fd = us->conn_fd[i];
                    if (other_fd > 0) {
                        rewind(f); 
                        packet_t req = { .type = PKT_UPLOAD_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(fn) + 1 };
                        memcpy(req.payload, fn, req.payload_size);
      
                        // Envia a requisição e espera pelo ACK
                        if (send_and_wait_ack(other_fd, &req) == 0) {                            
                            if (f == NULL) {
                                printf("Erro ao tentar abrir o arquivo '%s' para upload.\n", path);                              
                            }
                            
                            uint32_t seq = 2;
                            char buf[CHUNK_SIZE];
                            size_t n;
                            while ((n = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
                                packet_t dp = { .type = PKT_UPLOAD_DATA, .seq_num = seq++, .payload_size = (uint32_t)n };
                                memcpy(dp.payload, buf, n);
                                if (send_and_wait_ack(other_fd, &dp) != 0) {
                                    printf("Erro: Falha ao enviar chunk do arquivo ou receber ACK.\n");
                                }
                            }
                            // Verifica se o loop terminou devido a um erro de leitura do arquivo
                            if (ferror(f)) {
                                printf("Erro de leitura durante o upload do arquivo\n");
                            }

                            // Envia o pacote final indicando o fim do upload
                            packet_t endp = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0 };
                            send_packet(other_fd, &endp);
                            
                            printf("Arquivo '%s' atualizado com sucesso.\n", fn);                            
                        }                                                  
                        
                    } 
                }
                pthread_mutex_unlock(&sessions_mutex);
                fclose(f);
                break;
            }
            case PKT_DOWNLOAD_REQ: {
                printf("[*] Download: %s\n", fn);
                FILE *f = fopen(path, "rb");
                packet_t r = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
                send_packet(conn, &r);
                uint32_t seq = 1;
                while (!feof(f)) {
                    size_t n = fread(pkt.payload, 1, CHUNK_SIZE, f);
                    pkt.type = PKT_DOWNLOAD_DATA;
                    pkt.seq_num = seq++;
                    pkt.payload_size = (uint32_t)n;
                    send_packet(conn, &pkt);
                    recv_packet(conn, &r);
                }
                pkt.payload_size = 0;
                send_packet(conn, &pkt);
                fclose(f);
                break;
            }
            case PKT_DELETE_REQ: {
                printf("[*] Delete: %s\n", fn);
                remove(path);
                packet_t r2 = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
                send_packet(conn, &r2);
                break;
            }
            case PKT_LIST_SERVER_REQ: {
                DIR *d = opendir(base);
                struct dirent *e;
                struct stat st;
                char buf[CHUNK_SIZE];
                size_t off = 0;
                while ((e = readdir(d))) {
                    if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
                    char fp2[512];
                    snprintf(fp2, sizeof(fp2), "%s/%s", base, e->d_name);
                    if (stat(fp2, &st) < 0) continue;
                    off += snprintf(buf+off, CHUNK_SIZE-off,
                        "%s\t%ld bytes\tmtime:%ld\tatime:%ld\tctime:%ld\n",
                        e->d_name,
                        (long)st.st_size,
                        (long)st.st_mtime,
                        (long)st.st_atime,
                        (long)st.st_ctime);
                    if (off >= CHUNK_SIZE) break;
                }
                closedir(d);
                packet_t res = {
                    .type = PKT_LIST_SERVER_RES,
                    .seq_num = pkt.seq_num,
                    .payload_size = (uint32_t)off
                };
                memcpy(res.payload, buf, off);
                send_packet(conn, &res);
                break;
            }
            default:
                break;
        }
    }

    // End session
    pthread_mutex_lock(&sessions_mutex);
    us->count--;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (us->conn_fd[i] == conn) {
            us->conn_fd[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    close(conn);
    printf("[-] Sessão encerrada para '%s' (fd=%d) total=%d\n", user, conn, us->count);
    return NULL;
}

int main(void) {
    mkdir_p("storage", 0755);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(SERVER_PORT)
    };
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); exit(1); }
    printf("Server listening on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int *pfd = malloc(sizeof(int));
        *pfd = accept(listen_fd, (struct sockaddr*)&cli, &len);
        if (*pfd < 0) { perror("accept"); free(pfd); continue; }
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, pfd);
        pthread_detach(tid);
    }
    return 0;
}

