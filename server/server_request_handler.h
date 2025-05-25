#ifndef SERVER_REQUEST_HANDLER_H
#define SERVER_REQUEST_HANDLER_H

#include "../common/packet.h"
#include "server_session.h" // For UserSession_t

// Main dispatcher for packets received from a client
void handle_received_packet(int client_conn_fd, packet_t *pkt, UserSession_t *user_session, const char *user_storage_base_dir);

#endif // SERVER_REQUEST_HANDLER_H
