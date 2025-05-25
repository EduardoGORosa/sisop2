#ifndef CLIENT_ACTIONS_H
#define CLIENT_ACTIONS_H

#include "../common/packet.h"
#include <stdio.h> 
#include <pthread.h> 

#define CLIENT_MSG_SIZE 512

extern pthread_mutex_t socket_mutex; 

int send_and_wait_ack_client(int s, packet_t *p); 
char* upload_file_action(const char *full_path_arg, int sock);
char* delete_file_action(const char *filename, int sock);
void download_file_action(const char *filename, int sock, const char* initial_cwd); 
void list_server_files_action(int sock);
void list_client_files_action(void);

int download_file_to_sync_dir(const char *filename, long expected_size, int sock); 
int perform_initial_sync(int sock);
int remove_directory_recursively(const char *path);

#endif // CLIENT_ACTIONS_H
