#include <stdarg.h>
// server/server.c

#include "packet.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#define SERVER_PORT 12345
#define BACKLOG 10
#define CHUNK_SIZE MAX_PAYLOAD
#define MAX_USER_LEN 256
#define MAX_USER_CONNS 2  // Changed from 10 to 2 as per requirements
#define SOCKET_TIMEOUT 30 // 30 seconds timeout for socket operations
#define LOG_FILE "server.log"

// Mutex for file operations
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Log file
FILE *log_fp = NULL;

typedef struct UserSess {
  char username[MAX_USER_LEN];
  int conns[MAX_USER_CONNS];
  int conn_count;
  struct UserSess *next;
} UserSess;

static UserSess *sessions_head = NULL;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

// Logging function
void log_message(const char *format, ...) {
  if (!log_fp) {
    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp) return;
  }
  
  time_t now = time(NULL);
  char timestamp[64];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
  
  fprintf(log_fp, "[%s] ", timestamp);
  
  va_list args;
  va_start(args, format);
  vfprintf(log_fp, format, args);
  va_end(args);
  
  fprintf(log_fp, "\n");
  fflush(log_fp);
}

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
static int add_conn(UserSess *u, int fd) {
  if (u->conn_count >= MAX_USER_CONNS) {
    log_message("User %s already has maximum number of connections (%d)", u->username, MAX_USER_CONNS);
    return -1; // Maximum connections reached
  }
  
  u->conns[u->conn_count++] = fd;
  log_message("Added connection %d for user %s, total connections: %d", fd, u->username, u->conn_count);
  return 0;
}

// Remove a connection FD from a user's session list
static void remove_conn(UserSess *u, int fd) {
  for (int i = 0; i < u->conn_count; ++i) {
    if (u->conns[i] == fd) {
      u->conns[i] = u->conns[--u->conn_count];
      log_message("Removed connection %d for user %s, remaining connections: %d", fd, u->username, u->conn_count);
      return;
    }
  }
}

// Broadcast a packet to all of this user's other devices
static void broadcast_to_others(UserSess *u, packet_t *p, int from_fd) {
  for (int i = 0; i < u->conn_count; ++i) {
    int fd = u->conns[i];
    if (fd != from_fd) {
      if (send_packet(fd, p) != 0) {
        log_message("Failed to broadcast packet to fd %d", fd);
      } else {
        log_message("Broadcast packet to fd %d", fd);
      }
    }
  }
}

// Validate filename to prevent directory traversal attacks
static int validate_filename(const char *filename) {
  // Check for null or empty filename
  if (!filename || *filename == '\0') return 0;
  
  // Check for directory traversal attempts
  if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) return 0;
  
  return 1; // Valid filename
}

// Handle one client connection
void *handle_client(void *arg) {
  int conn = *(int *)arg;
  free(arg);
  packet_t pkt;
  char user[MAX_USER_LEN] = {0};
  UserSess *us = NULL;
  
  // Set up signal handler to catch thread termination
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  // 1) INITIAL HANDSHAKE: expect GET_SYNC_DIR
  if (recv_packet_timeout(conn, &pkt, SOCKET_TIMEOUT) < 0 || pkt.type != PKT_GET_SYNC_DIR) {
    log_message("Failed initial handshake: expected PKT_GET_SYNC_DIR");
    close(conn);
    return NULL;
  }
  
  // extract username
  size_t ulen =
      pkt.payload_size < MAX_USER_LEN - 1 ? pkt.payload_size : MAX_USER_LEN - 1;
  memcpy(user, pkt.payload, ulen);
  user[ulen] = '\0';
  
  // Validate username
  if (strlen(user) == 0) {
    log_message("Invalid username received");
    packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
    send_packet(conn, &nack);
    close(conn);
    return NULL;
  }

  // register session
  pthread_mutex_lock(&sessions_mutex);
  us = get_user_sess(user);
  int add_result = add_conn(us, conn);
  pthread_mutex_unlock(&sessions_mutex);
  
  // Check if maximum connections reached
  if (add_result < 0) {
    log_message("Maximum connections reached for user %s", user);
    packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
    send_packet(conn, &nack);
    close(conn);
    return NULL;
  }

  // create user directory
  char base[512];
  snprintf(base, sizeof(base), "storage/%s/sync_dir", user);
  mkdir_p("storage", 0755);
  mkdir_p(base, 0755);

  // send initial ACK
  packet_t ack = {.type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
  send_packet(conn, &ack);
  log_message("User %s connected (fd=%d), total devices=%d", user, conn, us->conn_count);

  // 2) COMMAND LOOP
  while (recv_packet_timeout(conn, &pkt, SOCKET_TIMEOUT) == 0) {
    // 2.2) CLI get_sync_dir
    if (pkt.type == PKT_GET_SYNC_DIR) {
      packet_t ack2 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
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
    
    // Validate filename
    if (!validate_filename(fn)) {
      log_message("Invalid filename received: %s", fn);
      packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
      send_packet(conn, &nack);
      continue;
    }
    
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, fn);

    switch (pkt.type) {
    case PKT_UPLOAD_REQ: {
      log_message("Upload request for file: %s", fn);
      // acknowledge request
      packet_t r1 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
      send_packet(conn, &r1);
      
      // receive data chunks
      pthread_mutex_lock(&file_mutex);
      FILE *f = fopen(path, "wb");
      if (!f) {
        log_message("Failed to open file for writing: %s (error: %s)", path, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
        send_packet(conn, &nack);
        continue;
      }
      
      uint32_t expected_seq = pkt.seq_num + 1;
      while (recv_packet_timeout(conn, &pkt, SOCKET_TIMEOUT) == 0 && pkt.type == PKT_UPLOAD_DATA) {
        // Check sequence number
        if (pkt.seq_num != expected_seq) {
          log_message("Sequence number mismatch: expected %u, got %u", expected_seq, pkt.seq_num);
          packet_t nack = {.type = PKT_NACK, .seq_num = expected_seq, .total_size = 1, .payload_size = 0};
          send_packet(conn, &nack);
          continue;
        }
        
        if (pkt.payload_size == 0) break; // End of file
        
        fwrite(pkt.payload, 1, pkt.payload_size, f);
        packet_t ca = {
            .type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
        send_packet(conn, &ca);
        expected_seq++;
      }
      fclose(f);
      pthread_mutex_unlock(&file_mutex);
      
      log_message("Upload completed for file: %s", fn);
      
      // propagate to other devices
      packet_t se = {.type = PKT_SYNC_EVENT,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(fn) + 1};
      memcpy(se.payload, fn, se.payload_size);
      broadcast_to_others(us, &se, conn);
      break;
    }
    case PKT_DELETE_REQ: {
      log_message("Delete request for file: %s", fn);
      pthread_mutex_lock(&file_mutex);
      int result = remove(path);
      pthread_mutex_unlock(&file_mutex);
      
      if (result != 0) {
        log_message("Failed to delete file: %s (error: %s)", path, strerror(errno));
      }
      
      packet_t r2 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
      send_packet(conn, &r2);
      
      // propagate delete
      packet_t se = {.type = PKT_SYNC_EVENT,
                     .seq_num = 1,
                     .total_size = 1,
                     .payload_size = (uint32_t)strlen(fn) + 1};
      memcpy(se.payload, fn, se.payload_size);
      broadcast_to_others(us, &se, conn);
      break;
    }
    case PKT_DOWNLOAD_REQ: {
      log_message("Download request for file: %s", fn);
      pthread_mutex_lock(&file_mutex);
      FILE *f = fopen(path, "rb");
      pthread_mutex_unlock(&file_mutex);
      
      if (!f) {
        log_message("File not found: %s", path);
        packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
        send_packet(conn, &nack);
        continue;
      }
      
      packet_t r3 = {
          .type = PKT_ACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
      send_packet(conn, &r3);
      
      // Get file size to set total_size
      fseek(f, 0, SEEK_END);
      long file_size = ftell(f);
      fseek(f, 0, SEEK_SET);
      
      uint32_t total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
      if (total_chunks == 0) total_chunks = 1; // At least one chunk for empty files
      
      uint32_t seq = 1;
      while (!feof(f)) {
        size_t n = fread(pkt.payload, 1, CHUNK_SIZE, f);
        pkt.type = PKT_DOWNLOAD_DATA;
        pkt.seq_num = seq++;
        pkt.total_size = total_chunks;
        pkt.payload_size = (uint32_t)n;
        send_packet(conn, &pkt);
        
        // Wait for ACK
        packet_t ack_pkt;
        if (recv_packet_timeout(conn, &ack_pkt, SOCKET_TIMEOUT) < 0 || ack_pkt.type != PKT_ACK) {
          log_message("Failed to receive ACK for chunk %u", pkt.seq_num);
          break;
        }
      }
      
      // Send empty packet to signal end of file
      pkt.payload_size = 0;
      send_packet(conn, &pkt);
      fclose(f);
      log_message("Download completed for file: %s", fn);
      break;
    }
    case PKT_LIST_SERVER_REQ: {
      log_message("List server request from user: %s", user);
      DIR *d = opendir(base);
      struct dirent *e;
      struct stat st;
      char buf[CHUNK_SIZE];
      size_t off = 0;
      
      if (!d) {
        log_message("Failed to open directory: %s", base);
        packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
        send_packet(conn, &nack);
        continue;
      }
      
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        char fp2[512];
        snprintf(fp2, sizeof(fp2), "%s/%s", base, e->d_name);
        
        pthread_mutex_lock(&file_mutex);
        int stat_result = stat(fp2, &st);
        pthread_mutex_unlock(&file_mutex);
        
        if (stat_result < 0)
          continue;
        
        // Format MAC times as required
        char mtime_str[20], atime_str[20], ctime_str[20];
        struct tm *t;
        
        t = localtime(&st.st_mtime);
        strftime(mtime_str, sizeof(mtime_str), "%F %T", t);
        
        t = localtime(&st.st_atime);
        strftime(atime_str, sizeof(atime_str), "%F %T", t);
        
        t = localtime(&st.st_ctime);
        strftime(ctime_str, sizeof(ctime_str), "%F %T", t);
        
        off += snprintf(buf + off, CHUNK_SIZE - off, 
                       "%-20s %8ld bytes  mtime:%s  atime:%s  ctime:%s\n",
                       e->d_name, (long)st.st_size, mtime_str, atime_str, ctime_str);
        
        if (off >= CHUNK_SIZE)
          break;
      }
      closedir(d);
      
      packet_t res = {.type = PKT_LIST_SERVER_RES,
                      .seq_num = pkt.seq_num,
                      .total_size = 1,
                      .payload_size = (uint32_t)off};
      memcpy(res.payload, buf, off);
      send_packet(conn, &res);
      break;
    }
    default:
      log_message("Unknown packet type received: %d", pkt.type);
      packet_t nack = {.type = PKT_NACK, .seq_num = pkt.seq_num, .total_size = 1, .payload_size = 0};
      send_packet(conn, &nack);
      break;
    }
  }

  // teardown
  pthread_mutex_lock(&sessions_mutex);
  remove_conn(us, conn);
  pthread_mutex_unlock(&sessions_mutex);
  close(conn);
  log_message("User %s disconnected (fd=%d), remaining devices=%d", user, conn, us->conn_count);
  return NULL;
}

// Signal handler for graceful shutdown
void handle_signal(int sig) {
  if (log_fp) {
    log_message("Received signal %d, shutting down server", sig);
    fclose(log_fp);
  }
  exit(0);
}

int main(void) {
  // Set up signal handlers
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  
  // Initialize log file
  log_fp = fopen(LOG_FILE, "a");
  if (!log_fp) {
    perror("Failed to open log file");
  } else {
    log_message("Server starting up");
  }
  
  mkdir_p("storage", 0755);

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    log_message("Socket creation failed: %s", strerror(errno));
    perror("socket");
    exit(1);
  }
  
  // Set socket options to allow reuse of address
  int opt = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    log_message("setsockopt failed: %s", strerror(errno));
    perror("setsockopt");
    exit(1);
  }
  
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(SERVER_PORT)};
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    log_message("Bind failed: %s", strerror(errno));
    perror("bind");
    exit(1);
  }
  if (listen(listen_fd, BACKLOG) < 0) {
    log_message("Listen failed: %s", strerror(errno));
    perror("listen");
    exit(1);
  }
  log_message("Server listening on port %d", SERVER_PORT);
  printf("Server listening on port %d\n", SERVER_PORT);

  while (1) {
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int *pfd = malloc(sizeof(int));
    *pfd = accept(listen_fd, (struct sockaddr *)&cli, &len);
    if (*pfd < 0) {
      log_message("Accept failed: %s", strerror(errno));
      perror("accept");
      free(pfd);
      continue;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(cli.sin_addr), client_ip, INET_ADDRSTRLEN);
    log_message("New connection from %s:%d", client_ip, ntohs(cli.sin_port));
    
    pthread_t tid;
    pthread_create(&tid, NULL, handle_client, pfd);
    pthread_detach(tid);
  }
  
  if (log_fp) {
    fclose(log_fp);
  }
  
  return 0;
}

