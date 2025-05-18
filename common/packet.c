#include "packet.h"
#include <unistd.h>
#include <arpa/inet.h>

int send_packet(int sockfd, const packet_t *pkt) {
    packet_t netpkt = *pkt;
    netpkt.seq_num      = htonl(pkt->seq_num);
    netpkt.payload_size = htonl(pkt->payload_size);
    ssize_t sent = write(sockfd, &netpkt, sizeof(netpkt));
    return (sent == sizeof(netpkt)) ? 0 : -1;
}

int recv_packet(int sockfd, packet_t *pkt) {
    ssize_t recvd = read(sockfd, pkt, sizeof(*pkt));
    if (recvd != sizeof(*pkt)) return -1;
    pkt->seq_num      = ntohl(pkt->seq_num);
    pkt->payload_size = ntohl(pkt->payload_size);
    return 0;
}

