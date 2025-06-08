#include "packet.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h> // For memcpy if not included elsewhere

int send_packet(int sockfd, const packet_t *pkt) {
    packet_t netpkt = *pkt;
    netpkt.seq_num      = htonl(pkt->seq_num);
    netpkt.payload_size = htonl(pkt->payload_size);
    ssize_t sent = write(sockfd, &netpkt, sizeof(packet_t)); // Ensure to send entire packet_t size
    return (sent == sizeof(packet_t)) ? 0 : -1;
}

int recv_packet(int sockfd, packet_t *pkt) {
    char *buffer = (char *)pkt; // Ponteiro para o início da estrutura do pacote
    size_t bytes_to_receive = sizeof(packet_t); // Ensure to receive entire packet_t size
    ssize_t bytes_received_total = 0;

    while (bytes_received_total < bytes_to_receive) {
        printf("Recebendo pacotes. Total: %i . Recebendo: %i \n", bytes_received_total , bytes_to_receive);
        ssize_t bytes_received_now = read(sockfd, buffer + bytes_received_total, bytes_to_receive - bytes_received_total);

        if (bytes_received_now == -1) {
            if (errno == EINTR) { // Chamada interrompida por um sinal, tente novamente
                printf("Chamada interrompida\n");
                continue;
            }
            printf("Erro na leitura\n");
            return -1; // Erro de leitura
        }

        if (bytes_received_now == 0) {
            // Conexão fechada pelo peer antes de todos os dados serem recebidos
            printf("Conexão fechada inesperadamente\n");            
            return -1; // Conexão fechada
        }
        bytes_received_total += bytes_received_now;
        printf("Bytes recebidos: %i\n", bytes_received_now);
    }

    // Se chegou aqui, todos os bytes foram recebidos
    pkt->seq_num      = ntohl(pkt->seq_num);
    pkt->payload_size = ntohl(pkt->payload_size);
    return 0;
}
