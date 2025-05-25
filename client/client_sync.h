#ifndef CLIENT_SYNC_H
#define CLIENT_SYNC_H

#include "../common/packet.h"
#include <limits.h> 
#include <sys/inotify.h> 
#include <pthread.h>

#define INOTIFY_BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

extern pthread_mutex_t socket_mutex; 

void *notify_file_change_thread(void *parameter);
void *server_updates_listener_thread(void *arg);
void handle_server_initiated_download(int sock, const char *filename, const char* sync_dir_abs_path);

#endif // CLIENT_SYNC_H
