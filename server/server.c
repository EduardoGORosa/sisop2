// server/server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For close
#include <arpa/inet.h>  // For sockaddr_in, inet_ntoa
#include <sys/socket.h> // For socket, bind, listen, accept
#include <pthread.h>    // For pthread_create, pthread_detach
#include <limits.h>
#include "../common/packet.h"
#include "server_utils.h"
#include "server_session.h"
#include "server_request_handler.h"

#define SERVER_DEFAULT_PORT 12345
#define SERVER_BACKLOG      10
#define STORAGE_BASE_DIR "storage"

typedef struct {
    int client_conn_fd;
    // struct sockaddr_in client_addr; // If needed for logging client IP
} client_handler_args_t;


void *client_handler_thread(void *arg) {
    client_handler_args_t *handler_args = (client_handler_args_t*)arg;
    int conn_fd = handler_args->client_conn_fd;
    // char client_ip_str[INET_ADDRSTRLEN];
    // inet_ntop(AF_INET, &handler_args->client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
    free(handler_args); // Free the arguments structure

    packet_t initial_pkt;
    if (recv_packet(conn_fd, &initial_pkt) < 0 || initial_pkt.type != PKT_GET_SYNC_DIR) {
        fprintf(stderr, "Falha ao receber pacote inicial ou tipo incorreto de fd=%d.\n", conn_fd);
        close(conn_fd);
        return NULL;
    }

    char username[MAX_USER_LEN];
    size_t ulen = initial_pkt.payload_size < (MAX_USER_LEN -1) ? initial_pkt.payload_size : (MAX_USER_LEN -1);
    memcpy(username, initial_pkt.payload, ulen);
    username[ulen > 0 ? ulen -1: 0] = '\0'; // Ensure null termination (if payload_size includes it)
    if (initial_pkt.payload_size > 0 && initial_pkt.payload_size <= MAX_USER_LEN) { // If not null terminated
         username[initial_pkt.payload_size] = '\0';
    } else if (initial_pkt.payload_size == 0){
         username[0] = '\0'; // Empty username
    }


    if (strlen(username) == 0) {
        fprintf(stderr, "Nome de usuário vazio recebido de fd=%d. Rejeitando.\n", conn_fd);
        packet_t nack_resp = { .type = PKT_NACK, .seq_num = initial_pkt.seq_num };
        snprintf(nack_resp.payload, MAX_PAYLOAD, "Nome de usuário não pode ser vazio.");
        nack_resp.payload_size = strlen(nack_resp.payload) +1;
        send_packet(conn_fd, &nack_resp);
        close(conn_fd);
        return NULL;
    }


    lock_sessions();
    UserSession_t *user_session = get_or_create_user_session_locked(username);
    if (!user_session) { // Should not happen if calloc worked
        unlock_sessions();
        fprintf(stderr, "Falha crítica ao obter/criar sessão para '%s'.\n", username);
        packet_t nack_resp = { .type = PKT_NACK, .seq_num = initial_pkt.seq_num };
         snprintf(nack_resp.payload, MAX_PAYLOAD, "Erro interno do servidor (sessão).");
        nack_resp.payload_size = strlen(nack_resp.payload) +1;
        send_packet(conn_fd, &nack_resp);
        close(conn_fd);
        return NULL;
    }

    if (add_connection_to_session_locked(user_session, conn_fd) != 0) {
        unlock_sessions();
        fprintf(stderr, "Usuário '%s' (fd=%d) excedeu o limite de conexões (%d).\n", username, conn_fd, MAX_SESSIONS_PER_USER);
        packet_t nack_resp = { .type = PKT_NACK, .seq_num = initial_pkt.seq_num };
        snprintf(nack_resp.payload, MAX_PAYLOAD, "Limite de conexões atingido.");
        nack_resp.payload_size = strlen(nack_resp.payload) +1;
        send_packet(conn_fd, &nack_resp);
        close(conn_fd);
        return NULL;
    }
    unlock_sessions();

    packet_t ack_resp = { .type = PKT_ACK, .seq_num = initial_pkt.seq_num, .payload_size = 0 };
    send_packet(conn_fd, &ack_resp);
    
    lock_sessions();
    printf("[+] Sessão iniciada para '%s' (fd=%d), total de conexões ativas para este usuário: %d\n",
           username, conn_fd, user_session->active_connections_count);
    unlock_sessions();


    char user_storage_base_dir[PATH_MAX];
    snprintf(user_storage_base_dir, sizeof(user_storage_base_dir), "%s/%s/sync_dir", STORAGE_BASE_DIR, username);
    mkdir_p(STORAGE_BASE_DIR, 0755); // Ensure base storage directory exists
    char user_base_for_mkdir[PATH_MAX]; // Path for user's own base before sync_dir
    snprintf(user_base_for_mkdir, sizeof(user_base_for_mkdir), "%s/%s", STORAGE_BASE_DIR, username);
    mkdir_p(user_base_for_mkdir, 0755); // Ensure user's directory exists
    mkdir_p(user_storage_base_dir, 0755); // Ensure sync_dir for user exists


    packet_t received_pkt;
    while (recv_packet(conn_fd, &received_pkt) == 0) {
        handle_received_packet(conn_fd, &received_pkt, user_session, user_storage_base_dir);
    }

    // Client disconnected or error in recv_packet
    printf("[-] Conexão com fd=%d (usuário '%s') encerrada ou perdida.\n", conn_fd, username);
    lock_sessions();
    remove_connection_from_session_locked(user_session, conn_fd);
    printf("[-] Sessão para '%s' (fd=%d) finalizada. Conexões restantes para este usuário: %d\n",
           username, conn_fd, user_session->active_connections_count);
    unlock_sessions();
    close(conn_fd);
    return NULL;
}


int main(int argc, char *argv[]) {
    int port = SERVER_DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Porta inválida: %s. Usando porta padrão %d.\n", argv[1], SERVER_DEFAULT_PORT);
            port = SERVER_DEFAULT_PORT;
        }
    }

    init_session_management(); // Initialize mutex for sessions
    mkdir_p(STORAGE_BASE_DIR, 0755); // Create base storage directory at startup

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket creation failed"); exit(EXIT_FAILURE); }

    // Allow address reuse
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        // Non-fatal, but good for development
    }


    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY, // Listen on all available interfaces
        .sin_port = htons(port)
    };

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, SERVER_BACKLOG) < 0) {
        perror("listen failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    printf("Servidor escutando na porta %d...\n", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);

        if (conn_fd < 0) {
            perror("accept failed");
            continue; // Continue to accept other connections
        }
        
        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
        printf("Nova conexão de %s:%d (fd=%d)\n", client_ip_str, ntohs(client_addr.sin_port), conn_fd);

        client_handler_args_t *thread_args = (client_handler_args_t*) malloc(sizeof(client_handler_args_t));
        if (!thread_args) {
            perror("malloc for thread_args failed");
            close(conn_fd);
            continue;
        }
        thread_args->client_conn_fd = conn_fd;
        // thread_args->client_addr = client_addr; // If needed by thread

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler_thread, thread_args) != 0) {
            perror("pthread_create failed");
            free(thread_args);
            close(conn_fd);
        } else {
            pthread_detach(tid); // Detach thread as we are not joining it
        }
    }

    close(listen_fd); // Never reached in this loop
    return 0;
}
