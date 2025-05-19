// server/server.c

#include "../common/packet.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SERVER_PORT 12345
#define BACKLOG 10
#define CHUNK_SIZE MAX_PAYLOAD
#define MAX_USER_LEN 256
#define MAX_USER_CONNS 10

typedef struct UserSess {
  char username[MAX_USER_LEN];
  int conns[MAX_USER_CONNS];
  int conn_count;
  struct UserSess *next;
} UserSess;

static UserSess *sessions_head = NULL;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

// Create directories recursively ("mkdir -p")
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

// Find or create a UserSess for this username
static UserSess *get_user_sess(const char *user) {
  for (UserSess *u = sessions_head; u; u = u->next) {
    if (strcmp(u->username, user) == 0)
      return u;
  }
  UserSess *u = calloc(1, sizeof(*u));
  strncpy(u->username, user, MAX_USER_LEN - 1);
  u->next = sessions_head;
  sessions_head = u;
  return u;
}

// Add a connection FD to a user's session list
static void add_conn(UserSess *u, int fd) {
  if (u->conn_count < MAX_USER_CONNS) {
    u->conns[u->conn_count++] = fd;
  }
}

// Remove a connection FD from a user's session list
static void remove_conn(UserSess *u, int fd) {
  for (int i = 0; i < u->conn_count; ++i) {
    if (u->conns[i] == fd) {
      u->conns[i] = u->conns[--u->conn_count];
      return;
    }
  }
}

// Broadcast a packet to all of this user's other devices
static void broadcast_to_others(UserSess *u, packet_t *p, int from_fd) {
  for (int i = 0; i < u->conn_count; ++i) {
    int fd = u->conns[i];
    if (fd != from_fd) {
      send_packet(fd, p);
    }
  }
}

// Handle one client connection
void *handle_client(void *arg) {
  int conn = *(int *)arg;
  free(arg);
  packet_t pkt;

  // 1) INITIAL HANDSHAKE: expect GET_SYNC_DIR
  if (recv_packet(conn, &pkt) < 0 || pkt.type != PKT_GET_SYNC_DIR) {
    close(conn);
    return NULL;
  }
  // extract username
  char user[MAX_USER_LEN];
  size_t ulen =
      pkt.payload_size < MAX_USER_LEN - 1 ? pkt.payload_size : MAX_USER_LEN - 1;
  memcpy(user, pkt.payload, ulen);
  user[ulen] = '\0';

  // register session
  pthread_mutex_lock(&sessions_mutex);
  UserSess *us = get_user_sess(user);
  add_conn(us, conn);
  pthread_mutex_unlock(&sessions_mutex);

  // create user directory
  char base[512];
  snprintf(base, sizeof(base), "storage/%s/sync_dir", user);
  mkdir_p("storage", 0755);
  mkdir_p(base, 0755);

  // send initial ACK
  packet_t ack = {.type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
  send_packet(conn, &ack);
  printf("[+] %s connected (fd=%d), total devices=%d\n", user, conn,
         us->conn_count);

  // 2) COMMAND LOOP
  while (recv_packet(conn, &pkt) == 0) {
    // 2.2) CLI get_sync_dir
    if (pkt.type == PKT_GET_SYNC_DIR) {
      packet_t ack2 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
      send_packet(conn, &ack2);
      continue;
    }
    // ignore sync events from server
    if (pkt.type == PKT_SYNC_EVENT) {
      // client shouldn't send this; just ignore
      continue;
    }

    // common filename extraction
    size_t fl = pkt.payload_size < CHUNK_SIZE ? pkt.payload_size : CHUNK_SIZE;
    char fn[CHUNK_SIZE + 1];
    memcpy(fn, pkt.payload, fl);
    fn[fl] = '\0';
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, fn);

    switch (pkt.type) {
    case PKT_UPLOAD_REQ: {
      // acknowledge request
      packet_t r1 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
      send_packet(conn, &r1);
      // receive data chunks
      FILE *f = fopen(path, "wb");
      while (recv_packet(conn, &pkt) == 0 && pkt.payload_size > 0) {
        fwrite(pkt.payload, 1, pkt.payload_size, f);
        packet_t ca = {
            .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
        send_packet(conn, &ca);
      }
      fclose(f);
      // propagate to other devices
      packet_t se = {.type = PKT_SYNC_EVENT,
                     .seq_num = 1,
                     .payload_size = (uint32_t)strlen(fn) + 1};
      memcpy(se.payload, fn, se.payload_size);
      broadcast_to_others(us, &se, conn);
      break;
    }
    case PKT_DELETE_REQ: {
      remove(path);
      packet_t r2 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
      send_packet(conn, &r2);
      // propagate delete
      packet_t se = {.type = PKT_SYNC_EVENT,
                     .seq_num = 1,
                     .payload_size = (uint32_t)strlen(fn) + 1};
      memcpy(se.payload, fn, se.payload_size);
      broadcast_to_others(us, &se, conn);
      break;
    }
    case PKT_DOWNLOAD_REQ: {
      FILE *f = fopen(path, "rb");
      packet_t r3 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .payload_size = 0};
      send_packet(conn, &r3);
      uint32_t seq = 1;
      while (!feof(f)) {
        size_t n = fread(pkt.payload, 1, CHUNK_SIZE, f);
        pkt.type = PKT_DOWNLOAD_DATA;
        pkt.seq_num = seq++;
        pkt.payload_size = (uint32_t)n;
        send_packet(conn, &pkt);
        recv_packet(conn, &r3);
      }
      pkt.payload_size = 0;
      send_packet(conn, &pkt);
      fclose(f);
      break;
    }
    case PKT_LIST_SERVER_REQ: {
      DIR *d = opendir(base);
      struct dirent *e;
      struct stat st;
      char buf[CHUNK_SIZE];
      size_t off = 0;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        char fp2[512];
        snprintf(fp2, sizeof(fp2), "%s/%s", base, e->d_name);
        if (stat(fp2, &st) < 0)
          continue;
        off +=
            snprintf(buf + off, CHUNK_SIZE - off, "%s\t%ld bytes\tmtime:%ld\n",
                     e->d_name, (long)st.st_size, (long)st.st_mtime);
        if (off >= CHUNK_SIZE)
          break;
      }
      closedir(d);
      packet_t res = {.type = PKT_LIST_SERVER_RES,
                      .seq_num = pkt.seq_num,
                      .payload_size = (uint32_t)off};
      memcpy(res.payload, buf, off);
      send_packet(conn, &res);
      break;
    }
    default:
      // unknown—ignore
      break;
    }
  }

  // teardown
  pthread_mutex_lock(&sessions_mutex);
  remove_conn(us, conn);
  pthread_mutex_unlock(&sessions_mutex);
  close(conn);
  printf("[-] %s disconnected (fd=%d), remaining devices=%d\n", user, conn,
         us->conn_count);
  return NULL;
}

int main(void) {
  mkdir_p("storage", 0755);

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    exit(1);
  }
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(SERVER_PORT)};
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    exit(1);
  }
  if (listen(listen_fd, BACKLOG) < 0) {
    perror("listen");
    exit(1);
  }
  printf("Server listening on port %d\n", SERVER_PORT);

  while (1) {
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int *pfd = malloc(sizeof(int));
    *pfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
    if (*pfd < 0) {
      perror("accept");
      free(pfd);
      continue;
    }
    pthread_t tid;
    pthread_create(&tid, NULL, handle_client, pfd);
    pthread_detach(tid);
  }
  return 0;
}
