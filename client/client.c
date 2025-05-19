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
#include <errno.h>
#include <signal.h>
#include <time.h>
#if defined(__linux__)
#include <sys/inotify.h>
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))
#endif
#include "common/packet.h"

#define CHUNK_SIZE MAX_PAYLOAD
#define SOCKET_TIMEOUT 30 // 30 seconds timeout for socket operations
#define MAX_RETRIES 3     // Maximum number of retries for failed operations

static int sock_fd;
static int running = 1;
static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

// Validate filename to prevent directory traversal attacks
static int validate_filename(const char *filename) {
  // Check for null or empty filename
  if (!filename || *filename == '\0') return 0;
  
  // Check for directory traversal attempts
  if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) return 0;
  
  return 1; // Valid filename
}

// Envia p e espera um ACK, ignorando SYNC_EVENTs que possam chegar no meio
// Improved with timeout and retry mechanism
int send_and_wait_ack(int s, packet_t *p) {
  int retries = 0;
  
  while (retries < MAX_RETRIES) {
    pthread_mutex_lock(&socket_mutex);
    int send_result = send_packet(s, p);
    pthread_mutex_unlock(&socket_mutex);
    
    if (send_result != 0) {
      fprintf(stderr, "Failed to send packet (retry %d/%d)\n", retries + 1, MAX_RETRIES);
      retries++;
      sleep(1); // Wait before retry
      continue;
    }
    
    packet_t a;
    time_t start_time = time(NULL);
    
    while (time(NULL) - start_time < SOCKET_TIMEOUT) {
      pthread_mutex_lock(&socket_mutex);
      int recv_result = recv_packet_timeout(s, &a, 1); // 1 second timeout for quick response
      pthread_mutex_unlock(&socket_mutex);
      
      if (recv_result < 0) {
        // Timeout or error, continue waiting
        continue;
      }
      
      if (a.type == PKT_ACK) {
        return 0; // Success
      }
      
      if (a.type == PKT_NACK) {
        fprintf(stderr, "Received NACK from server\n");
        return -1; // Explicit rejection
      }
      
      if (a.type == PKT_SYNC_EVENT) {
        // Ignora; a receiver_thread vai lidar com isso
        continue;
      }
      
      if (a.type == PKT_ERROR) {
        fprintf(stderr, "Error from server: %s\n", a.payload);
        return -1;
      }
      // Ignora outros tipos
    }
    
    // Timeout waiting for ACK
    fprintf(stderr, "Timeout waiting for ACK (retry %d/%d)\n", retries + 1, MAX_RETRIES);
    retries++;
  }
  
  fprintf(stderr, "Failed to get ACK after %d retries\n", MAX_RETRIES);
  return -1;
}

// Faz download completo de um arquivo 'fn' do servidor
// Improved with error handling and validation
void auto_download(const char *fn) {
  if (!validate_filename(fn)) {
    fprintf(stderr, "[sync] Invalid filename: %s\n", fn);
    return;
  }

  // 1) solicita download
  packet_t rq = {.type = PKT_DOWNLOAD_REQ,
                 .seq_num = 1,
                 .total_size = 1,
                 .payload_size = (uint32_t)strlen(fn) + 1};
  memcpy(rq.payload, fn, rq.payload_size);
  
  pthread_mutex_lock(&socket_mutex);
  int send_result = send_packet(sock_fd, &rq);
  pthread_mutex_unlock(&socket_mutex);
  
  if (send_result < 0) {
    fprintf(stderr, "[sync] Failed to send download request for '%s'\n", fn);
    return;
  }

  // 2) espera ACK
  packet_t resp;
  pthread_mutex_lock(&socket_mutex);
  int recv_result = recv_packet_timeout(sock_fd, &resp, SOCKET_TIMEOUT);
  pthread_mutex_unlock(&socket_mutex);
  
  if (recv_result < 0 || resp.type != PKT_ACK) {
    fprintf(stderr, "[sync] Failed to receive ACK for download '%s'\n", fn);
    return;
  }

  // 3) recebe chunks
  FILE *fp = fopen(fn, "wb");
  if (!fp) {
    perror("[sync] fopen");
    return;
  }
  
  uint32_t expected_seq = 1;
  uint32_t total_chunks = 0;
  
  while (1) {
    packet_t dp;
    pthread_mutex_lock(&socket_mutex);
    recv_result = recv_packet_timeout(sock_fd, &dp, SOCKET_TIMEOUT);
    pthread_mutex_unlock(&socket_mutex);
    
    if (recv_result < 0) {
      fprintf(stderr, "[sync] Failed to receive data packet for '%s'\n", fn);
      break;
    }
    
    if (dp.type != PKT_DOWNLOAD_DATA) {
      fprintf(stderr, "[sync] Unexpected packet type: %d\n", dp.type);
      break;
    }
    
    // Check sequence number
    if (dp.seq_num != expected_seq) {
      fprintf(stderr, "[sync] Sequence number mismatch: expected %u, got %u\n", 
              expected_seq, dp.seq_num);
      
      // Send NACK
      packet_t nack = {.type = PKT_NACK, .seq_num = expected_seq, 
                       .total_size = 1, .payload_size = 0};
      pthread_mutex_lock(&socket_mutex);
      send_packet(sock_fd, &nack);
      pthread_mutex_unlock(&socket_mutex);
      continue;
    }
    
    // Update total chunks if this is the first packet
    if (expected_seq == 1) {
      total_chunks = dp.total_size;
    }
    
    if (dp.payload_size == 0)
      break; // fim
      
    fwrite(dp.payload, 1, dp.payload_size, fp);
    
    // ACK de chunk
    packet_t ca = {.type = PKT_ACK, .seq_num = dp.seq_num, 
                  .total_size = 1, .payload_size = 0};
    pthread_mutex_lock(&socket_mutex);
    send_packet(sock_fd, &ca);
    pthread_mutex_unlock(&socket_mutex);
    
    expected_seq++;
  }
  
  fclose(fp);
  printf("[sync] '%s' atualizado.\n", fn);
}

// Thread que consome PKT_SYNC_EVENT e chama auto_download
void *receiver_thread(void *arg) {
  (void)arg;
  packet_t pkt;
  
  while (running) {
    pthread_mutex_lock(&socket_mutex);
    int recv_result = recv_packet_timeout(sock_fd, &pkt, 1); // 1 second timeout for responsiveness
    pthread_mutex_unlock(&socket_mutex);
    
    if (recv_result < 0) {
      // Timeout or error, check if we should continue
      if (!running) break;
      continue;
    }
    
    if (pkt.type == PKT_SYNC_EVENT) {
      char fn[CHUNK_SIZE + 1];
      size_t len =
          pkt.payload_size < CHUNK_SIZE ? pkt.payload_size : CHUNK_SIZE;
      memcpy(fn, pkt.payload, len);
      fn[len] = '\0';
      
      if (validate_filename(fn)) {
        auto_download(fn);
      } else {
        fprintf(stderr, "[sync] Invalid filename in sync event: %s\n", fn);
      }
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
  while (running) {
    int length = read(fd, buffer, EVENT_BUF_LEN);
    if (length <= 0) {
      if (!running) break;
      usleep(500000);
      continue;
    }

    int i = 0;
    while (i < length) {
      struct inotify_event *evt = (struct inotify_event *)&buffer[i];
      if (!(evt->mask & IN_ISDIR)) {
        char *name = evt->name;
        
        // Validate filename
        if (!validate_filename(name)) {
          fprintf(stderr, "[auto-sync] Invalid filename: %s\n", name);
          i += EVENT_SIZE + evt->len;
          continue;
        }
        
        // create/modify → upload
        if (evt->mask & (IN_CREATE | IN_MODIFY)) {
          packet_t req = {.type = PKT_UPLOAD_REQ,
                          .seq_num = 1,
                          .total_size = 1,
                          .payload_size = (uint32_t)strlen(name) + 1};
          memcpy(req.payload, name, req.payload_size);
          if (send_and_wait_ack(sock_fd, &req) == 0) {
            FILE *fp = fopen(name, "rb");
            if (fp) {
              // Get file size to set total_size
              fseek(fp, 0, SEEK_END);
              long file_size = ftell(fp);
              fseek(fp, 0, SEEK_SET);
              
              uint32_t total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
              if (total_chunks == 0) total_chunks = 1; // At least one chunk for empty files
              
              uint32_t seq = 2;
              char buf[CHUNK_SIZE];
              size_t n;
              while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
                packet_t dp = {.type = PKT_UPLOAD_DATA,
                               .seq_num = seq++,
                               .total_size = total_chunks,
                               .payload_size = (uint32_t)n};
                memcpy(dp.payload, buf, n);
                if (send_and_wait_ack(sock_fd, &dp) != 0) {
                  fprintf(stderr, "[auto-sync] Failed to upload chunk for '%s'\n", name);
                  break;
                }
              }
              packet_t endp = {
                  .type = PKT_UPLOAD_DATA, 
                  .seq_num = seq, 
                  .total_size = total_chunks,
                  .payload_size = 0};
              pthread_mutex_lock(&socket_mutex);
              send_packet(sock_fd, &endp);
              pthread_mutex_unlock(&socket_mutex);
              fclose(fp);
              printf("[auto-sync] '%s' uploaded.\n", name);
            } else {
              perror("[auto-sync] fopen");
            }
          } else {
            fprintf(stderr, "[auto-sync] Failed to initiate upload for '%s'\n", name);
          }
        }
        // delete → envia delete
        else if (evt->mask & IN_DELETE) {
          packet_t rq = {.type = PKT_DELETE_REQ,
                         .seq_num = 1,
                         .total_size = 1,
                         .payload_size = (uint32_t)strlen(name) + 1};
          memcpy(rq.payload, name, rq.payload_size);
          if (send_and_wait_ack(sock_fd, &rq) == 0) {
            printf("[auto-sync] '%s' deleted.\n", name);
          } else {
            fprintf(stderr, "[auto-sync] Failed to delete '%s'\n", name);
          }
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

// Signal handler for graceful shutdown
void handle_signal(int sig) {
  printf("\nReceived signal %d, shutting down...\n", sig);
  running = 0;
  close(sock_fd);
  exit(0);
}

int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Uso: %s <user> <host> <port>\n", argv[0]);
    return 1;
  }
  const char *user = argv[1];
  const char *host = argv[2];
  int port = atoi(argv[3]);
  
  // Validate user input
  if (strlen(user) == 0) {
    fprintf(stderr, "Error: Username cannot be empty\n");
    return 1;
  }
  
  if (port <= 0 || port > 65535) {
    fprintf(stderr, "Error: Invalid port number\n");
    return 1;
  }

  // Set up signal handlers
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  // Prepara diretório local
  mkdir("sync_dir", 0755);
  if (chdir("sync_dir") != 0) {
    perror("chdir");
    return 1;
  }

  // Conecta ao servidor
  sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in serv = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, host, &serv.sin_addr) <= 0) {
    fprintf(stderr, "Invalid address: %s\n", host);
    close(sock_fd);
    return 1;
  }
  
  printf("Connecting to %s:%d...\n", host, port);
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
                   .total_size = 1,
                   .payload_size = (uint32_t)strlen(user) + 1};
  memcpy(init.payload, user, init.payload_size);
  if (send_and_wait_ack(sock_fd, &init) == 0) {
    printf("Sessão iniciada para '%s'\n", user);
    printf("Diretório sync_dir inicializado no servidor.\n");
  } else {
    fprintf(stderr, "Falha ao inicializar sync_dir no servidor.\n");
    running = 0;
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
  while (running && printf("> "), fgets(line, sizeof(line), stdin)) {
    char *cmd = strtok(line, " \t\n");
    if (!cmd)
      continue;

    if (strcmp(cmd, "get_sync_dir") == 0) {
      packet_t rq = {.type = PKT_GET_SYNC_DIR,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(user) + 1};
      memcpy(rq.payload, user, rq.payload_size);
      if (send_and_wait_ack(sock_fd, &rq) == 0) {
        printf("sync_dir (re)inicializado.\n");
      } else {
        fprintf(stderr, "Falha ao reinicializar sync_dir.\n");
      }
    } else if (strcmp(cmd, "upload") == 0) {
      char *path = strtok(NULL, " \t\n");
      if (!path) {
        fprintf(stderr, "Uso: upload <file>\n");
        continue;
      }
      
      // Check if file exists
      struct stat st;
      if (stat(path, &st) != 0) {
        fprintf(stderr, "Arquivo não encontrado: %s\n", strerror(errno));
        continue;
      }
      
      char *filename = basename(path);
      
      // Validate filename
      if (!validate_filename(filename)) {
        fprintf(stderr, "Nome de arquivo inválido: %s\n", filename);
        continue;
      }
      
      packet_t rq = {.type = PKT_UPLOAD_REQ,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(filename) + 1};
      memcpy(rq.payload, filename, rq.payload_size);
      if (send_and_wait_ack(sock_fd, &rq) == 0) {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
          perror("fopen");
        } else {
          // Get file size to set total_size
          fseek(fp, 0, SEEK_END);
          long file_size = ftell(fp);
          fseek(fp, 0, SEEK_SET);
          
          uint32_t total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
          if (total_chunks == 0) total_chunks = 1; // At least one chunk for empty files
          
          uint32_t seq = 2;
          char buf[CHUNK_SIZE];
          size_t n;
          int upload_success = 1;
          
          while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
            packet_t dp = {.type = PKT_UPLOAD_DATA,
                           .seq_num = seq++,
                           .total_size = total_chunks,
                           .payload_size = (uint32_t)n};
            memcpy(dp.payload, buf, n);
            if (send_and_wait_ack(sock_fd, &dp) != 0) {
              fprintf(stderr, "Falha ao enviar chunk do arquivo.\n");
              upload_success = 0;
              break;
            }
          }
          
          if (upload_success) {
            packet_t endp = {
                .type = PKT_UPLOAD_DATA, 
                .seq_num = seq, 
                .total_size = total_chunks,
                .payload_size = 0};
            pthread_mutex_lock(&socket_mutex);
            send_packet(sock_fd, &endp);
            pthread_mutex_unlock(&socket_mutex);
            printf("Upload de '%s' concluído com sucesso.\n", filename);
          }
          
          fclose(fp);
        }
      } else {
        fprintf(stderr, "Falha ao iniciar upload.\n");
      }
    } else if (strcmp(cmd, "download") == 0) {
      char *f = strtok(NULL, " \t\n");
      if (!f) {
        fprintf(stderr, "Uso: download <file>\n");
        continue;
      }
      
      // Validate filename
      if (!validate_filename(f)) {
        fprintf(stderr, "Nome de arquivo inválido: %s\n", f);
        continue;
      }
      
      packet_t rq = {.type = PKT_DOWNLOAD_REQ,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(f) + 1};
      memcpy(rq.payload, f, rq.payload_size);
      
      pthread_mutex_lock(&socket_mutex);
      send_packet(sock_fd, &rq);
      pthread_mutex_unlock(&socket_mutex);

      packet_t r;
      pthread_mutex_lock(&socket_mutex);
      int recv_result = recv_packet_timeout(sock_fd, &r, SOCKET_TIMEOUT);
      pthread_mutex_unlock(&socket_mutex);
      
      if (recv_result < 0 || r.type != PKT_ACK) {
        fprintf(stderr, "Falha no ACK para download '%s'\n", f);
        continue;
      }

      FILE *fp = fopen(f, "wb");
      if (!fp) {
        perror("fopen");
        continue;
      }

      uint32_t expected_seq = 1;
      uint32_t total_chunks = 0;
      int download_success = 1;
      
      while (1) {
        packet_t dp;
        pthread_mutex_lock(&socket_mutex);
        recv_result = recv_packet_timeout(sock_fd, &dp, SOCKET_TIMEOUT);
        pthread_mutex_unlock(&socket_mutex);
        
        if (recv_result < 0) {
          fprintf(stderr, "Falha ao receber dados do arquivo.\n");
          download_success = 0;
          break;
        }
        
        if (dp.type != PKT_DOWNLOAD_DATA) {
          fprintf(stderr, "Tipo de pacote inesperado: %d\n", dp.type);
          download_success = 0;
          break;
        }
        
        // Check sequence number
        if (dp.seq_num != expected_seq) {
          fprintf(stderr, "Número de sequência incorreto: esperado %u, recebido %u\n", 
                  expected_seq, dp.seq_num);
          
          // Send NACK
          packet_t nack = {.type = PKT_NACK, .seq_num = expected_seq, 
                          .total_size = 1, .payload_size = 0};
          pthread_mutex_lock(&socket_mutex);
          send_packet(sock_fd, &nack);
          pthread_mutex_unlock(&socket_mutex);
          continue;
        }
        
        // Update total chunks if this is the first packet
        if (expected_seq == 1) {
          total_chunks = dp.total_size;
        }
        
        if (dp.payload_size == 0)
          break; // End of file
          
        fwrite(dp.payload, 1, dp.payload_size, fp);
        
        packet_t ca = {.type = PKT_ACK, .seq_num = dp.seq_num, 
                      .total_size = 1, .payload_size = 0};
        pthread_mutex_lock(&socket_mutex);
        send_packet(sock_fd, &ca);
        pthread_mutex_unlock(&socket_mutex);
        
        expected_seq++;
      }
      
      fclose(fp);
      
      if (download_success) {
        printf("Download de '%s' concluído com sucesso.\n", f);
      } else {
        fprintf(stderr, "Download de '%s' falhou.\n", f);
        remove(f); // Remove incomplete file
      }
    } else if (strcmp(cmd, "delete") == 0) {
      char *f = strtok(NULL, " \t\n");
      if (!f) {
        fprintf(stderr, "Uso: delete <file>\n");
        continue;
      }
      
      // Validate filename
      if (!validate_filename(f)) {
        fprintf(stderr, "Nome de arquivo inválido: %s\n", f);
        continue;
      }
      
      packet_t rq = {.type = PKT_DELETE_REQ,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(f) + 1};
      memcpy(rq.payload, f, rq.payload_size);
      if (send_and_wait_ack(sock_fd, &rq) == 0) {
        printf("'%s' deletado.\n", f);
      } else {
        fprintf(stderr, "Falha ao deletar '%s'.\n", f);
      }
    } else if (strcmp(cmd, "list_server") == 0) {
      packet_t rq = {
          .type = PKT_LIST_SERVER_REQ, 
          .seq_num = 1, 
          .total_size = 1,
          .payload_size = 0};
      
      pthread_mutex_lock(&socket_mutex);
      send_packet(sock_fd, &rq);
      pthread_mutex_unlock(&socket_mutex);
      
      packet_t res;
      pthread_mutex_lock(&socket_mutex);
      int recv_result = recv_packet_timeout(sock_fd, &res, SOCKET_TIMEOUT);
      pthread_mutex_unlock(&socket_mutex);
      
      if (recv_result == 0 && res.type == PKT_LIST_SERVER_RES) {
        printf("Arquivos no servidor:\n");
        printf("%-20s %8s  %-19s  %-19s  %-19s\n", 
               "Nome", "Tamanho", "Modificação", "Acesso", "Criação");
        printf("--------------------------------------------------------------------------------\n");
        fwrite(res.payload, 1, res.payload_size, stdout);
      } else {
        fprintf(stderr, "Falha ao listar arquivos do servidor.\n");
      }
    } else if (strcmp(cmd, "list_client") == 0) {
      printf("Arquivos no cliente:\n");
      printf("%-20s %8s  %-19s  %-19s  %-19s\n", 
             "Nome", "Tamanho", "Modificação", "Acesso", "Criação");
      printf("--------------------------------------------------------------------------------\n");
      
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
      printf("Encerrando sessão...\n");
      break;
    } else {
      printf("Comando desconhecido\n");
    }
  }

  // Signal threads to stop
  running = 0;
  
  // Wait for threads to finish (with timeout)
  time_t start = time(NULL);
  while (time(NULL) - start < 3) {
    usleep(100000); // 100ms
  }
  
  close(sock_fd);
  return 0;
}

