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
#include <sys/inotify.h>
#include <pthread.h>

#define CHUNK_SIZE MAX_PAYLOAD
#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

int send_and_wait_ack(int s, packet_t *p) {
    send_packet(s, p);
    packet_t a;
    return (recv_packet(s, &a) == 0 && a.type == PKT_ACK) ? 0 : -1;
}

char* upload(char *full_path_arg , int sock) {
  char *msg = malloc(512);
  if (full_path_arg) {
      char *base_filename = strrchr(full_path_arg, '/'); // Encontra a última ocorrência de '/'
      if (base_filename == NULL) {
          // Nenhuma '/' encontrada, o próprio argumento é o nome do arquivo (ex: "arquivo.txt")
          base_filename = full_path_arg;
      } else {
          // A '/' foi encontrada, avança o ponteiro para depois da '/' para obter o nome do arquivo
          base_filename++;
      }

      // Verificar se o nome do arquivo base não é muito longo
      if (strlen(base_filename) + 1 > MAX_PAYLOAD) {
          snprintf(msg, 256, "Erro: Nome do arquivo '%s' é muito longo.", base_filename);
          return msg;
      }
      // Prepara o pacote de requisição de upload com o NOME BASE do arquivo
      packet_t rq = { .type = PKT_UPLOAD_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(base_filename) + 1 };
      memcpy(rq.payload, base_filename, rq.payload_size);
      
      // Envia a requisição e espera pelo ACK
      if (send_and_wait_ack(sock, &rq) == 0) {
          // Abre o arquivo usando o CAMINHO COMPLETO fornecido pelo usuário
          FILE *fp = fopen(full_path_arg, "rb");
          if (fp == NULL) {
              snprintf(msg, 512, "Erro ao tentar abrir o arquivo '%s' para upload.", full_path_arg);
              return msg;
          }

          // O restante da lógica de leitura do arquivo e envio dos pacotes de dados permanece o mesmo
          uint32_t seq = 2;
          char buf[CHUNK_SIZE];
          size_t n;
          while ((n = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
              packet_t dp = { .type = PKT_UPLOAD_DATA, .seq_num = seq++, .payload_size = (uint32_t)n };
              memcpy(dp.payload, buf, n);
              if (send_and_wait_ack(sock, &dp) != 0) {
                  msg = "Erro: Falha ao enviar chunk do arquivo ou receber ACK.\n";
                  return msg;
              }
          }
          // Verifica se o loop terminou devido a um erro de leitura do arquivo
          if (ferror(fp)) {
              msg = "Erro de leitura durante o upload do arquivo";
              return msg;
          }

          // Envia o pacote final indicando o fim do upload
          packet_t endp = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0 };
          send_packet(sock, &endp); // Para este pacote final, um ACK não é esperado pelo send_and_wait_ack
                                    // mas o servidor envia um ACK para cada chunk, incluindo o de 0 bytes no original
                                    // Se for necessário aguardar ACK para o pacote de 0 bytes:
                                    // send_and_wait_ack(sock, &endp);
                                    // O código original do server.c parece esperar um ACK para o pacote de 0 bytes
                                    // no entanto, a lógica de upload do cliente original envia um pacote final sem esperar ACK
                                    // Por consistência com o fluxo atual do server.c, é melhor enviar e esperar ACK ou ajustar o server.
                                    // O server.c:
                                    // while (recv_packet(conn, &pkt) == 0 && pkt.payload_size > 0) {
                                    //    fwrite(pkt.payload, 1, pkt.payload_size, f);
                                    //    packet_t ca = { .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0 };
                                    //    send_packet(conn, &ca);
                                    // }
                                    // O servidor não envia ACK para o pacote de 0 bytes, ele sai do loop.
                                    // Portanto, o cliente não deve esperar ACK para o pacote final de 0 bytes.

          fclose(fp);
          snprintf(msg, 512, "Arquivo '%s' enviado com sucesso (como '%s' para o servidor).", full_path_arg, base_filename);
      } else {
          snprintf(msg, 256, "Erro: Servidor não confirmou o pedido de upload para '%s'.", base_filename);
      }
  } else {
      msg = "Uso: upload <caminho/completo/do/arquivo.ext>";
  }
  return msg;
}

char* delete(char *file , int sock) {
  if (file) {
      packet_t rq = { .type = PKT_DELETE_REQ, .seq_num = 1, .payload_size = (uint32_t)strlen(file)+1 };
      memcpy(rq.payload, file, rq.payload_size);
      send_packet(sock, &rq);
      packet_t r; recv_packet(sock, &r);
  }
  else{
      return "Erro: Nome do arquivo para exclusão não especificado.";
  }
}

void notify_file_change(void *parameter) {
    int sock = *(int*)parameter;
    char buf[BUF_LEN];
    int inotify_fd = 0;
    struct inotify_event *event = NULL;
    char* pathname = "../sync_dir";
    char abs_path[PATH_MAX];
    realpath(pathname, abs_path);

    inotify_fd = inotify_init();
    inotify_add_watch(inotify_fd, pathname, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE );
    while (1) {
        int n = read(inotify_fd, buf, BUF_LEN);
        char* p = buf;
        while (p < buf + n) {
            event = (struct inotify_event*)p;
            uint32_t mask = event->mask;
            
            char full_path[PATH_MAX];
            snprintf(full_path, PATH_MAX, "%s/%s", abs_path, event->name);
            
            if(event->name[0] != '.'){    // ignora arquivos de metadados gerados por aplicativos de edição de texto
                if (mask & IN_CREATE) {
                    upload( full_path, sock );
                }
                if (mask & IN_DELETE) {
                    delete( event->name , sock );
                }
                if (mask & IN_CLOSE_WRITE) {
                    upload( full_path, sock );
                }
            }

            p += sizeof(struct inotify_event) + event->len;
        }
    }
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

    pthread_t thing1;
    int *parameter = malloc(sizeof(int));
    *parameter = sock;
    pthread_create(&thing1, NULL, notify_file_change, (void *)parameter);

    char line[256];
    while (printf("> "), fgets(line, sizeof(line), stdin)) {
        char *cmd = strtok(line, " \t\n");
        if (!cmd) continue;

        if (strcmp(cmd, "upload") == 0) {
            char *full_path_arg = strtok(NULL, " \t\n"); // Lê o caminho completo fornecido pelo usuário
            char *msg = upload( full_path_arg , sock );
            if (msg) {
                printf("%s\n", msg);
                free(msg);
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
            char *file = strtok(NULL, " \t\n");
            char msg = delete( file , sock );
            if (msg) {
                printf("%s\n", msg);
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

