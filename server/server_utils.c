#include "server_utils.h"
#include <stdio.h>  // For snprintf, perror
#include <string.h> // For strlen
#include <stdlib.h> // For exit (if needed, though not directly used here)
#include <sys/stat.h> // For mkdir
#include <unistd.h> // For access (optional, for checking before mkdir)
#include <errno.h>  // Added for errno and EEXIST


void mkdir_p(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;

    // Remove trailing slash if any
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) { // Check for EEXIST
                //perror("mkdir_p: mkdir error (intermediate)");
                // Optionally, handle error more gracefully
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) { // Check for EEXIST
        //perror("mkdir_p: mkdir error (final)");
    }
}

int send_and_wait_ack_server(int s, packet_t *p) {
    if (send_packet(s, p) != 0) {
        // perror("send_packet failed in send_and_wait_ack_server");
        return -1;
    }
    packet_t a;
    if (recv_packet(s, &a) != 0) {
        // perror("recv_packet failed in send_and_wait_ack_server");
        return -1;
    }
    return (a.type == PKT_ACK) ? 0 : -1;
}
