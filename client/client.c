// client/client.c

#include "../common/packet.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_SIZE MAX_PAYLOAD
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

int sock_fd; // socket descriptor shared with sync thread

int send_and_wait_ack(int s, packet_t *p) {
  send_packet(s, p);
  packet_t a;
  return (recv_packet(s, &a) == 0 && a.type == PKT_ACK) ? 0 : -1;
}

void *sync_thread(void *arg) {
  int fd = inotify_init();
  if (fd < 0) {
    perror("inotify_init");
    return NULL;
  }

  int wd = inotify_add_watch(fd, ".", IN_CREATE | IN_MODIFY | IN_DELETE);
  if (wd < 0) {
    perror("inotify_add_watch");
    close(fd);
    return NULL;
  }

  char buffer[EVENT_BUF_LEN];
  while (1) {
    int length = read(fd, buffer, EVENT_BUF_LEN);
    if (length < 0)
      break;

    int i = 0;
    while (i < length) {
      struct inotify_event *evt = (struct inotify_event *)&buffer[i];
      if (!(evt->mask & IN_ISDIR)) {
        // Created or modified: upload
        if (evt->mask & (IN_CREATE | IN_MODIFY)) {
          char *name = evt->name;
          // send upload request
          packet_t req = {.type = PKT_UPLOAD_REQ,
                          .seq_num = 1,
                          .payload_size = (uint32_t)strlen(name) + 1};
          memcpy(req.payload, name, req.payload_size);
          if (send_and_wait_ack(sock_fd, &req) == 0) {
            FILE *fp = fopen(name, "rb");
            if (fp) {
              uint32_t seq = 2;
              char buf[CHUNK_SIZE];
              size_t n;
              while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
                packet_t dp = {.type = PKT_UPLOAD_DATA,
                               .seq_num = seq++,
                               .payload_size = (uint32_t)n};
                memcpy(dp.payload, buf, n);
                send_and_wait_ack(sock_fd, &dp);
              }
              packet_t endp = {
                  .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0};
              send_packet(sock_fd, &endp);
              fclose(fp);
            }
          }
        }
        // Deleted: tell server
        else if (evt->mask & IN_DELETE) {
          char *name = evt->name;
          packet_t rq = {.type = PKT_DELETE_REQ,
                         .seq_num = 1,
                         .payload_size = (uint32_t)strlen(name) + 1};
          memcpy(rq.payload, name, rq.payload_size);
          send_packet(sock_fd, &rq);
          packet_t resp;
          recv_packet(sock_fd, &resp);
        }
      }
      i += EVENT_SIZE + evt->len;
    }
  }

  inotify_rm_watch(fd, wd);
  close(fd);
  return NULL;
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

  sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in serv = {.sin_family = AF_INET, .sin_port = htons(port)};
  inet_pton(AF_INET, host, &serv.sin_addr);

  if (connect(sock_fd, (void *)&serv, sizeof(serv)) < 0) {
    perror("connect");
    close(sock_fd);
    return 1;
  }

  // 1. Envia automaticamente o get_sync_dir
  packet_t init = {.type = PKT_GET_SYNC_DIR,
                   .seq_num = 1,
                   .payload_size = (uint32_t)strlen(user) + 1};
  memcpy(init.payload, user, init.payload_size);
  if (send_and_wait_ack(sock_fd, &init) == 0) {
    printf("Sessão iniciada para '%s'\n", user);
    printf("Diretório sync_dir inicializado no servidor.\n");
  } else {
    fprintf(stderr, "Falha ao inicializar sync_dir no servidor.\n");
  }

  printf("Comandos:\n"
         " get_sync_dir\n"
         " upload <file>\n"
         " download <file>\n"
         " delete <file>\n"
         " list_server\n"
         " list_client\n"
         " exit\n");

  // 3. Inicia thread de sync automático
  pthread_t tid;
  if (pthread_create(&tid, NULL, sync_thread, NULL) != 0) {
    perror("pthread_create");
  }

  char line[256];
  while (printf("> "), fgets(line, sizeof(line), stdin)) {
    char *cmd = strtok(line, " \t\n");
    if (!cmd)
      continue;

    if (strcmp(cmd, "get_sync_dir") == 0) {
      packet_t rq = {.type = PKT_GET_SYNC_DIR,
                     .seq_num = 1,
                     .payload_size = (uint32_t)strlen(user) + 1};
      memcpy(rq.payload, user, rq.payload_size);
      if (send_and_wait_ack(sock_fd, &rq) == 0) {
        printf("Diretório sync_dir (re)inicializado.\n");
      }
    } else if (strcmp(cmd, "upload") == 0) {
      char *path = strtok(NULL, " \t\n");
      if (path) {
        char *filename = basename(path);
        packet_t rq = {.type = PKT_UPLOAD_REQ,
                       .seq_num = 1,
                       .payload_size = (uint32_t)strlen(filename) + 1};
        memcpy(rq.payload, filename, rq.payload_size);
        if (send_and_wait_ack(sock_fd, &rq) == 0) {
          FILE *fp = fopen(path, "rb");
          if (!fp) {
            perror("fopen");
          } else {
            uint32_t seq = 2;
            char buf[CHUNK_SIZE];
            size_t n;
            while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
              packet_t dp = {.type = PKT_UPLOAD_DATA,
                             .seq_num = seq++,
                             .payload_size = (uint32_t)n};
              memcpy(dp.payload, buf, n);
              send_and_wait_ack(sock_fd, &dp);
            }
            packet_t endp = {
                .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0};
            send_packet(sock_fd, &endp);
            fclose(fp);
          }
        }
      }
    } else if (strcmp(cmd, "download") == 0) {
      char *f = strtok(NULL, " \t\n");
      if (f) {
        packet_t rq = {.type = PKT_DOWNLOAD_REQ,
                       .seq_num = 1,
                       .payload_size = (uint32_t)strlen(f) + 1};
        memcpy(rq.payload, f, rq.payload_size);
        send_packet(sock_fd, &rq);

        packet_t r;
        recv_packet(sock_fd, &r);
        FILE *fp = fopen(f, "wb");
        if (!fp) {
          perror("fopen");
          continue;
        }

        while (1) {
          packet_t dp;
          recv_packet(sock_fd, &dp);
          if (dp.payload_size == 0)
            break;
          fwrite(dp.payload, 1, dp.payload_size, fp);
          packet_t ca = {
              .type = PKT_ACK, .seq_num = dp.seq_num, .payload_size = 0};
          send_packet(sock_fd, &ca);
        }
        fclose(fp);
      }
    } else if (strcmp(cmd, "delete") == 0) {
      char *f = strtok(NULL, " \t\n");
      if (f) {
        packet_t rq = {.type = PKT_DELETE_REQ,
                       .seq_num = 1,
                       .payload_size = (uint32_t)strlen(f) + 1};
        memcpy(rq.payload, f, rq.payload_size);
        send_packet(sock_fd, &rq);
        packet_t r;
        recv_packet(sock_fd, &r);
      }
    } else if (strcmp(cmd, "list_server") == 0) {
      packet_t rq = {
          .type = PKT_LIST_SERVER_REQ, .seq_num = 1, .payload_size = 0};
      send_packet(sock_fd, &rq);
      packet_t res;
      recv_packet(sock_fd, &res);
      fwrite(res.payload, 1, res.payload_size, stdout);
    } else if (strcmp(cmd, "list_client") == 0) {
      struct stat st;
      DIR *d = opendir(".");
      struct dirent *e;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        if (stat(e->d_name, &st) < 0)
          continue;

        char mtime[20], atime[20], ctime_buf[20];
        struct tm *tm_info;

        tm_info = localtime(&st.st_mtime);
        strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M:%S", tm_info);
        tm_info = localtime(&st.st_atime);
        strftime(atime, sizeof(atime), "%Y-%m-%d %H:%M:%S", tm_info);
        tm_info = localtime(&st.st_ctime);
        strftime(ctime_buf, sizeof(ctime_buf), "%Y-%m-%d %H:%M:%S", tm_info);

        printf("%-20s %8ld bytes  mtime:%s  atime:%s  ctime:%s\n", e->d_name,
               (long)st.st_size, mtime, atime, ctime_buf);
      }
      closedir(d);
    } else if (strcmp(cmd, "exit") == 0) {
      break;
    } else {
      printf("Comando desconhecido\n");
    }
  }

  close(sock_fd);
  return 0;
}

