#include "packet.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

int send_packet(int sockfd, const packet_t *pkt) {
    packet_t netpkt = *pkt;
    netpkt.seq_num      = htonl(pkt->seq_num);
    netpkt.total_size   = htonl(pkt->total_size);  // Added total_size conversion
    netpkt.payload_size = htonl(pkt->payload_size);
    ssize_t sent = write(sockfd, &netpkt, sizeof(netpkt));
    return (sent == sizeof(netpkt)) ? 0 : -1;
}

int recv_packet(int sockfd, packet_t *pkt) {
    ssize_t recvd = read(sockfd, pkt, sizeof(*pkt));
    if (recvd != sizeof(*pkt)) return -1;
    pkt->seq_num      = ntohl(pkt->seq_num);
    pkt->total_size   = ntohl(pkt->total_size);  // Added total_size conversion
    pkt->payload_size = ntohl(pkt->payload_size);
    return 0;
}

// Added timeout version of recv_packet
int recv_packet_timeout(int sockfd, packet_t *pkt, int timeout_sec) {
    fd_set readfds;
    struct timeval tv;
    int flags, ret;
    
    // Save current socket flags
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) return -1;
    
    // Set socket to non-blocking mode
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    
    // Set up select parameters
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    
    // Wait for data or timeout
    ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    
    // Restore original socket flags
    fcntl(sockfd, F_SETFL, flags);
    
    if (ret == -1) {
        // Select error
        return -1;
    } else if (ret == 0) {
        // Timeout
        errno = ETIMEDOUT;
        return -1;
    }
    
    // Data is available, read it
    return recv_packet(sockfd, pkt);
}

// Added function to create error packet
void create_error_packet(packet_t *pkt, uint32_t seq_num, const char *error_msg) {
    pkt->type = PKT_ERROR;
    pkt->seq_num = seq_num;
    pkt->total_size = 1;  // Single packet error message
    pkt->payload_size = strlen(error_msg) + 1;
    
    if (pkt->payload_size > MAX_PAYLOAD) {
        pkt->payload_size = MAX_PAYLOAD;
    }
    
    strncpy(pkt->payload, error_msg, pkt->payload_size - 1);
    pkt->payload[pkt->payload_size - 1] = '\0';
}

