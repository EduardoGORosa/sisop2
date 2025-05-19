// client/client.c

#include <arpa/inet.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))
#endif
#include "../common/packet.h"

#define CHUNK_SIZE MAX_PAYLOAD

static int sock_fd;

// Envia p e espera um ACK, ignorando SYNC_EVENTs que possam chegar no meio
int send_and_wait_ack(int s, packet_t *p) {
  send_packet(s, p);
  packet_t a;
  while (recv_packet(s, &a) == 0) {
    if (a.type == PKT_ACK) {
      return 0;
    }
    if (a.type == PKT_SYNC_EVENT) {
      // Ignora; a receiver_thread vai lidar com isso
      continue;
    }
    // Ignora outros tipos
  }
  return -1;
}

// Faz download completo de um arquivo 'fn' do servidor
void auto_download(const char *fn) {
  // 1) solicita download
  packet_t rq = {.type = PKT_DOWNLOAD_REQ,
                 .seq_num = 1,
                 .payload_size = (uint32_t)strlen(fn) + 1};
  memcpy(rq.payload, fn, rq.payload_size);
  send_packet(sock_fd, &rq);

  // 2) espera ACK
  packet_t resp;
  if (recv_packet(sock_fd, &resp) < 0 || resp.type != PKT_ACK) {
    fprintf(stderr, "[sync] falha no ACK do download '%s'\n", fn);
    return;
  }

  // 3) recebe chunks
  FILE *fp = fopen(fn, "wb");
  if (!fp) {
    perror("[sync] fopen");
    return;
  }
  while (1) {
    packet_t dp;
    if (recv_packet(sock_fd, &dp) < 0)
      break;
    if (dp.payload_size == 0)
      break; // fim
    fwrite(dp.payload, 1, dp.payload_size, fp);
    // ACK de chunk
    packet_t ca = {.type = PKT_ACK, .seq_num = dp.seq_num, .payload_size = 0};
    send_packet(sock_fd, &ca);
  }
  fclose(fp);
  printf("[sync] '%s' atualizado.\n", fn);
}

// Thread que consome PKT_SYNC_EVENT e chama auto_download
void *receiver_thread(void *arg) {
  (void)arg;
  packet_t pkt;
  while (recv_packet(sock_fd, &pkt) == 0) {
    if (pkt.type == PKT_SYNC_EVENT) {
      char fn[CHUNK_SIZE + 1];
      size_t len =
          pkt.payload_size < CHUNK_SIZE ? pkt.payload_size : CHUNK_SIZE;
      memcpy(fn, pkt.payload, len);
      fn[len] = '\0';
      auto_download(fn);
    }
  }
  return NULL;
}

// Thread de inotify para auto‐upload (só no Linux)
void *sync_thread(void *arg) {
  (void)arg;
#if defined(__linux__)
  int fd = inotify_init1(IN_NONBLOCK);
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
    if (length <= 0) {
      usleep(500000);
      continue;
    }

    int i = 0;
    while (i < length) {
      struct inotify_event *evt = (struct inotify_event *)&buffer[i];
      if (!(evt->mask & IN_ISDIR)) {
        char *name = evt->name;
        // create/modify → upload
        if (evt->mask & (IN_CREATE | IN_MODIFY)) {
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
        // delete → envia delete
        else if (evt->mask & IN_DELETE) {
          packet_t rq = {.type = PKT_DELETE_REQ,
                         .seq_num = 1,
                         .payload_size = (uint32_t)strlen(name) + 1};
          memcpy(rq.payload, name, rq.payload_size);
          send_and_wait_ack(sock_fd, &rq);
        }
      }
      i += EVENT_SIZE + evt->len;
    }
  }

  inotify_rm_watch(fd, wd);
  close(fd);
#else
  fprintf(stderr, "[auto-sync] não é suportado neste SO\n");
#endif
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

  // Prepara diretório local
  mkdir("sync_dir", 0755);
  chdir("sync_dir");

  // Conecta ao servidor
  sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in serv = {.sin_family = AF_INET, .sin_port = htons(port)};
  inet_pton(AF_INET, host, &serv.sin_addr);
  if (connect(sock_fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
    perror("connect");
    close(sock_fd);
    return 1;
  }

  // Inicia thread de recepção de eventos
  pthread_t recv_tid;
  if (pthread_create(&recv_tid, NULL, receiver_thread, NULL) != 0) {
    perror("pthread_create receiver");
    close(sock_fd);
    return 1;
  }

  // Inicia thread de auto‐upload (Linux only)
  pthread_t sync_tid;
  if (pthread_create(&sync_tid, NULL, sync_thread, NULL) != 0) {
    perror("pthread_create sync");
    // continua mesmo sem auto‐upload
  }

  // Handshake GET_SYNC_DIR
  packet_t init = {.type = PKT_GET_SYNC_DIR,
                   .seq_num = 1,
                   .payload_size = (uint32_t)strlen(user) + 1};
  memcpy(init.payload, user, init.payload_size);
  if (send_and_wait_ack(sock_fd, &init) == 0) {
    printf("Sessão iniciada para '%s'\n", user);
    printf("Diretório sync_dir inicializado no servidor.\n");
  } else {
    fprintf(stderr, "Falha ao inicializar sync_dir no servidor.\n");
    close(sock_fd);
    return 1;
  }

  // Loop de comandos
  printf("Comandos:\n"
         " get_sync_dir\n"
         " upload <file>\n"
         " download <file>\n"
         " delete <file>\n"
         " list_server\n"
         " list_client\n"
         " exit\n");

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
        printf("sync_dir (re)inicializado.\n");
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
        if (recv_packet(sock_fd, &r) < 0 || r.type != PKT_ACK) {
          fprintf(stderr, "falha no ACK para download '%s'\n", f);
          continue;
        }

        FILE *fp = fopen(f, "wb");
        if (!fp) {
          perror("fopen");
          continue;
        }

        while (1) {
          packet_t dp;
          if (recv_packet(sock_fd, &dp) < 0)
            break;
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
        if (send_and_wait_ack(sock_fd, &rq) == 0) {
          printf("'%s' deletado.\n", f);
        }
      }
    } else if (strcmp(cmd, "list_server") == 0) {
      packet_t rq = {
          .type = PKT_LIST_SERVER_REQ, .seq_num = 1, .payload_size = 0};
      send_packet(sock_fd, &rq);
      packet_t res;
      if (recv_packet(sock_fd, &res) == 0 && res.type == PKT_LIST_SERVER_RES) {
        fwrite(res.payload, 1, res.payload_size, stdout);
      }
    } else if (strcmp(cmd, "list_client") == 0) {
      struct stat st;
      DIR *d = opendir(".");
      struct dirent *e;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        if (stat(e->d_name, &st) < 0)
          continue;
        char m[20], a[20], c[20];
        struct tm *t;
        t = localtime(&st.st_mtime);
        strftime(m, sizeof(m), "%F %T", t);
        t = localtime(&st.st_atime);
        strftime(a, sizeof(a), "%F %T", t);
        t = localtime(&st.st_ctime);
        strftime(c, sizeof(c), "%F %T", t);
        printf("%-20s %8ld bytes  mtime:%s  atime:%s  ctime:%s\n", e->d_name,
               (long)st.st_size, m, a, c);
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
