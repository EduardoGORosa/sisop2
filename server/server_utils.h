#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

#include <sys/stat.h> // For mode_t
#include "../common/packet.h" // For packet_t

// mkdir -p equivalent
void mkdir_p(const char *path, mode_t mode);

// Server-specific send and wait for ACK
int send_and_wait_ack_server(int s, packet_t *p);


#endif // SERVER_UTILS_H
