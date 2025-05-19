// client/client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../common/packet.h"

#define CHUNK_SIZE MAX_PAYLOAD

int send_and_wait_ack(int s, packet_t *p) {
    send_packet(s, p);
    packet_t a;
    return (recv_packet(s, &a) == 0 && a.type == PKT_ACK) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <user> <host> <port>\n", argv[0]);
        return 1;
    }
    const char *user = argv[1];
    const char *host = argv[2];
    int port = atoi(argv[3]);

    mkdir("sync_dir", 0755);
    chdir("sync_dir");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, host, &serv.sin_addr);
    if (connect(sock, (void*)&serv, sizeof(serv)) < 0) {
        perror("connect");
        return 1;
    }

    packet_t init = { .type = PKT_GET_SYNC_DIR, .seq_num = 1, .payload_size = (uint32_t)strlen(user)+1 };
    memcpy(init.payload, user, init.payload_size);
    send_packet(sock, &init);
    packet_t ack;
    recv_packet(sock, &ack);

    printf("Sessão iniciada para '%s'\n", user);
    printf("Comandos:\n"
           " upload <file>\n"
           " download <file>\n"
           " delete <file>\n"
           " list_server\n"
           " list_client\n"
           " exit\n");

    char line[256];
    while (printf("> "), fgets(line, sizeof(line), stdin)) {
        char *cmd = strtok(line, " \t\n");
        if (!cmd) continue;

        if (strcmp(cmd, "upload") == 0) {
            char *f = strtok(NULL, " \t\n");
            if (f) {
                packet_t rq = { .type = PKT_UPLOAD_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(f)+1 };
                memcpy(rq.payload, f, rq.payload_size);
                if (send_and_wait_ack(sock, &rq) == 0) {
                    FILE *fp = fopen(f, "rb");
                    uint32_t seq = 2;
                    char buf[CHUNK_SIZE];
                    size_t n;
                    while ((n = fread(buf,1,CHUNK_SIZE,fp)) > 0) {
                        packet_t dp = { .type = PKT_UPLOAD_DATA, .seq_num = seq++, .payload_size = (uint32_t)n };
                        memcpy(dp.payload, buf, n);
                        send_and_wait_ack(sock, &dp);
                    }
                    packet_t endp = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0 };
                    send_packet(sock, &endp);
                    fclose(fp);
                }
            }
        }
        else if (strcmp(cmd, "download") == 0) {
            char *f = strtok(NULL, " \t\n");
            if (f) {
                packet_t rq = { .type = PKT_DOWNLOAD_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(f)+1 };
                memcpy(rq.payload, f, rq.payload_size);
                send_packet(sock, &rq);
                packet_t r; recv_packet(sock, &r);
                FILE *fp = fopen(f, "wb");
                while (1) {
                    packet_t dp;
                    recv_packet(sock, &dp);
                    if (dp.payload_size == 0) break;
                    fwrite(dp.payload,1,dp.payload_size,fp);
                    packet_t ca = { .type = PKT_ACK, .seq_num = dp.seq_num, .payload_size = 0 };
                    send_packet(sock, &ca);
                }
                fclose(fp);
            }
        }
        else if (strcmp(cmd, "delete") == 0) {
            char *f = strtok(NULL, " \t\n");
            if (f) {
                packet_t rq = { .type = PKT_DELETE_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(f)+1 };
                memcpy(rq.payload, f, rq.payload_size);
                send_packet(sock, &rq);
                packet_t r; recv_packet(sock, &r);
            }
        }
        else if (strcmp(cmd, "list_server") == 0) {
            packet_t rq = { .type = PKT_LIST_SERVER_REQ, .seq_num = 1, .payload_size = 0 };
            send_packet(sock, &rq);
            packet_t res; recv_packet(sock, &res);
            fwrite(res.payload,1,res.payload_size,stdout);
        }
        else if (strcmp(cmd, "list_client") == 0) {
            struct stat st;
            DIR *d = opendir(".");
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
                stat(e->d_name, &st);
                printf("%s\t%ld bytes\n", e->d_name, (long)st.st_size);
            }
            closedir(d);
        }
        else if (strcmp(cmd, "exit") == 0) {
            break;
        }
        else {
            printf("Comando desconhecido\n");
        }
    }

    close(sock);
    return 0;
}

