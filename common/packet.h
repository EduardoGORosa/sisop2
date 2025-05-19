#ifndef COMMON_PACKET_H
#define COMMON_PACKET_H

#include <stdint.h>

#define MAX_PAYLOAD 4096

typedef enum {
    PKT_UPLOAD_REQ,
    PKT_UPLOAD_DATA,
    PKT_DOWNLOAD_REQ,
    PKT_DOWNLOAD_DATA,
    PKT_DELETE_REQ,
    PKT_LIST_SERVER_REQ,
    PKT_LIST_SERVER_RES,
    PKT_LIST_CLIENT_REQ,
    PKT_SYNC_EVENT,
    PKT_GET_SYNC_DIR,  // ← novo
    PKT_ACK,
    PKT_NACK,
    PKT_ERROR          // New packet type for error reporting
} packet_type_t;

typedef struct {
    packet_type_t type;
    uint32_t      seq_num;
    uint32_t      total_size;    // Added total_size field as suggested in requirements
    uint32_t      payload_size;
    char          payload[MAX_PAYLOAD];
} packet_t;

// Protótipos para envio/recepção
int send_packet(int sockfd, const packet_t *pkt);
int recv_packet(int sockfd, packet_t *pkt);

// Added timeout version of recv_packet
int recv_packet_timeout(int sockfd, packet_t *pkt, int timeout_sec);

// Added function to create error packet
void create_error_packet(packet_t *pkt, uint32_t seq_num, const char *error_msg);

#endif // COMMON_PACKET_H

