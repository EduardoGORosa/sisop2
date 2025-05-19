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

typedef struct UserSess {
    char username[MAX_USER_LEN];
    int  count;
    int  conn_fd;
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
    u->conn_fd = -1;
    u->next = sessions_head;
    sessions_head = u;
    return u;
}

void *handle_client(void *arg) {
    int conn = *(int*)arg;
    free(arg);
    packet_t pkt;

    // 1) GET_SYNC_DIR
    if (recv_packet(conn, &pkt) < 0 || pkt.type != PKT_GET_SYNC_DIR) {
        close(conn);
        return NULL;
    }

    // Copy user safely
    char user[MAX_USER_LEN];
    size_t ulen = pkt.payload_size < (MAX_USER_LEN - 1)
                   ? pkt.payload_size
                   : (MAX_USER_LEN - 1);
    memcpy(user, pkt.payload, ulen);
    user[ulen] = '\0';

    // 2) Session control
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
    us->conn_fd = conn;
    pthread_mutex_unlock(&sessions_mutex);

    // 3) Initial ACK
    packet_t ack = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
    send_packet(conn, &ack);
    printf("[+] Sessão iniciada para '%s' (fd=%d) total=%d\n",
           user, conn, us->count);

    // 4) Prepare directory
    char base[512];
    snprintf(base, sizeof(base), "storage/%s/sync_dir", user);
    mkdir_p("storage", 0755);
    mkdir_p(base, 0755);

    // 5) Command loop
    while (recv_packet(conn, &pkt) == 0) {
        if (pkt.type == PKT_SYNC_EVENT) {
            continue;
        }

        // Extract filename safely
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
    if (us->conn_fd == conn) us->conn_fd = -1;
    pthread_mutex_unlock(&sessions_mutex);
    close(conn);
    printf("[-] Sessão encerrada para '%s' (fd=%d) total=%d\n",
           user, conn, us->count);
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

