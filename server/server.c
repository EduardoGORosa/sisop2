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

static UserSess *get_user_sess(const char *user) {
    for (UserSess *u = sessions_head; u; u = u->next)
        if (strcmp(u->username, user) == 0) return u;
    UserSess *u = calloc(1, sizeof(UserSess));
    strncpy(u->username, user, MAX_USER_LEN);
    u->conn_fd = -1;
    u->next = sessions_head;
    sessions_head = u;
    return u;
}

static void broadcast_sync(const char *user, packet_t *evt_pkt, int origin_fd) {
    pthread_mutex_lock(&sessions_mutex);
    for (UserSess *u = sessions_head; u; u = u->next) {
        if (strcmp(u->username, user)==0 &&
            u->conn_fd != -1 &&
            u->conn_fd != origin_fd)
        {
            send_packet(u->conn_fd, evt_pkt);
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}

void *handle_client(void *arg) {
    int conn = *(int*)arg; free(arg);
    packet_t pkt;

    // 1) GET_SYNC_DIR
    if (recv_packet(conn, &pkt) < 0 || pkt.type != PKT_GET_SYNC_DIR) {
        close(conn);
        return NULL;
    }
    char user[MAX_USER_LEN];
    memcpy(user, pkt.payload, pkt.payload_size);
    user[pkt.payload_size - 1] = '\0';

    // 2) session control (max 2)
    pthread_mutex_lock(&sessions_mutex);
    UserSess *us = get_user_sess(user);
    if (us->count >= 2) {
        pthread_mutex_unlock(&sessions_mutex);
        packet_t nack = { .type=PKT_NACK, .seq_num=pkt.seq_num, .payload_size=0 };
        send_packet(conn, &nack);
        close(conn);
        return NULL;
    }
    us->count++;
    us->conn_fd = conn;
    pthread_mutex_unlock(&sessions_mutex);

    // 3) ACK
    packet_t ack = { .type=PKT_ACK, .seq_num=pkt.seq_num, .payload_size=0 };
    send_packet(conn, &ack);

    // 4) prepare storage/<user>/sync_dir
    char user_dir[512], sync_dir[512];
    snprintf(user_dir, sizeof(user_dir),   "storage/%s",      user);
    snprintf(sync_dir, sizeof(sync_dir),   "%s/sync_dir", user_dir);
    mkdir("storage", 0755);
    mkdir(user_dir,   0755);
    mkdir(sync_dir,   0755);

    // 5) main loop
    while (recv_packet(conn, &pkt) == 0) {
        if (pkt.type == PKT_SYNC_EVENT) {
            // skip
            continue;
        }
        char filename[CHUNK_SIZE];
        memcpy(filename, pkt.payload, pkt.payload_size);
        filename[pkt.payload_size - 1] = '\0';
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", sync_dir, filename);

        switch (pkt.type) {
          case PKT_UPLOAD_REQ: {
            FILE *fp = fopen(path, "wb");
            packet_t r = { .type=PKT_ACK, .seq_num=pkt.seq_num, .payload_size=0 };
            send_packet(conn, &r);
            while (recv_packet(conn, &pkt)==0 && pkt.payload_size>0) {
                fwrite(pkt.payload,1,pkt.payload_size,fp);
                packet_t ca = { .type=PKT_ACK, .seq_num=pkt.seq_num, .payload_size=0 };
                send_packet(conn, &ca);
            }
            fclose(fp);
            // broadcast upload
            size_t L = strlen(filename) + 2;
            packet_t evt = { .type=PKT_SYNC_EVENT, .seq_num=0, .payload_size=(uint32_t)L };
            evt.payload[0] = 'U';
            strcpy(evt.payload+1, filename);
            broadcast_sync(user, &evt, conn);
            break;
          }
          case PKT_DOWNLOAD_REQ: {
            FILE *fp = fopen(path, "rb");
            packet_t r = { .type=PKT_ACK, .seq_num=pkt.seq_num, .payload_size=0 };
            send_packet(conn, &r);
            uint32_t seq = 1;
            while (!feof(fp)) {
                size_t n = fread(pkt.payload,1,CHUNK_SIZE,fp);
                pkt.type = PKT_DOWNLOAD_DATA;
                pkt.seq_num = seq++;
                pkt.payload_size = (uint32_t)n;
                send_packet(conn, &pkt);
                recv_packet(conn, &r);
            }
            pkt.payload_size = 0;
            send_packet(conn, &pkt);
            fclose(fp);
            break;
          }
          case PKT_DELETE_REQ: {
            remove(path);
            packet_t r2 = { .type=PKT_ACK, .seq_num=pkt.seq_num, .payload_size=0 };
            send_packet(conn, &r2);
            // broadcast delete
            size_t L2 = strlen(filename) + 2;
            packet_t evt2 = { .type=PKT_SYNC_EVENT, .seq_num=0, .payload_size=(uint32_t)L2 };
            evt2.payload[0] = 'D';
            strcpy(evt2.payload+1, filename);
            broadcast_sync(user, &evt2, conn);
            break;
          }
          case PKT_LIST_SERVER_REQ: {
            DIR *d = opendir(sync_dir);
            struct dirent *e;
            struct stat st;
            char buf[CHUNK_SIZE]; size_t off=0;
            while ((e=readdir(d))) {
                if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
                char fp_[512];
                snprintf(fp_,sizeof(fp_),"%s/%s",sync_dir,e->d_name);
                stat(fp_,&st);
                off += snprintf(buf+off, CHUNK_SIZE-off,
                    "%s\t%ld bytes\tmtime:%ld\tatime:%ld\tctime:%ld\n",
                    e->d_name,(long)st.st_size,
                    (long)st.st_mtime,(long)st.st_atime,(long)st.st_ctime);
                if (off>=CHUNK_SIZE) break;
            }
            closedir(d);
            packet_t res = { .type=PKT_LIST_SERVER_RES, .seq_num=pkt.seq_num, .payload_size=(uint32_t)off };
            memcpy(res.payload, buf, off);
            send_packet(conn, &res);
            break;
          }
          default:
            break;
        }
    }

    // cleanup
    pthread_mutex_lock(&sessions_mutex);
    us->count--;
    if (us->conn_fd == conn) us->conn_fd = -1;
    pthread_mutex_unlock(&sessions_mutex);
    close(conn);
    return NULL;
}

int main(void) {
    mkdir("storage", 0755);
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(SERVER_PORT)
    };
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, BACKLOG);
    while (1) {
        struct sockaddr_in cli; socklen_t len = sizeof(cli);
        int *connfd = malloc(sizeof(int));
        *connfd = accept(listen_fd, (struct sockaddr*)&cli, &len);
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, connfd);
        pthread_detach(tid);
    }
    return 0;
}

