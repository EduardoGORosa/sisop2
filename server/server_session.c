#include "server_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UserSession_t *sessions_head = NULL;
static pthread_mutex_t sessions_mutex;

void init_session_management(void) {
    pthread_mutex_init(&sessions_mutex, NULL);
}

void lock_sessions(void) {
    pthread_mutex_lock(&sessions_mutex);
}

void unlock_sessions(void) {
    pthread_mutex_unlock(&sessions_mutex);
}

UserSession_t* find_session_by_username_locked(const char* username) {
    for (UserSession_t *u = sessions_head; u; u = u->next) {
        if (strcmp(u->username, username) == 0) return u;
    }
    return NULL;
}


UserSession_t *get_or_create_user_session_locked(const char *username) {
    UserSession_t *session = find_session_by_username_locked(username);
    if (session) {
        return session;
    }

    // Create new session
    session = (UserSession_t*) calloc(1, sizeof(UserSession_t));
    if (!session) {
        perror("calloc for UserSession_t failed");
        return NULL; // Critical error
    }
    strncpy(session->username, username, MAX_USER_LEN - 1);
    session->username[MAX_USER_LEN - 1] = '\0'; // Ensure null termination
    session->active_connections_count = 0;
    for (int i = 0; i < MAX_SESSIONS_PER_USER; i++) {
        session->connection_fds[i] = 0; // 0 indicates slot is free
    }
    session->next = sessions_head;
    sessions_head = session;
    return session;
}

int add_connection_to_session_locked(UserSession_t *session, int conn_fd) {
    if (!session) return -1;

    if (session->active_connections_count >= MAX_SESSIONS_PER_USER) {
        return -1; // Session full
    }

    for (int i = 0; i < MAX_SESSIONS_PER_USER; i++) {
        if (session->connection_fds[i] == 0) { // Find an empty slot
            session->connection_fds[i] = conn_fd;
            session->active_connections_count++;
            return 0; // Success
        }
    }
    return -1; // Should not happen if count is correct, but as a safeguard
}

void remove_connection_from_session_locked(UserSession_t *session, int conn_fd) {
    if (!session) return;

    for (int i = 0; i < MAX_SESSIONS_PER_USER; i++) {
        if (session->connection_fds[i] == conn_fd) {
            session->connection_fds[i] = 0; // Mark slot as free
            session->active_connections_count--;
            break;
        }
    }
    // Note: Session cleanup (freeing UserSession_t if count is 0) is not done here.
    // It could be added if desired, but makes finding sessions more complex.
    // For simplicity, sessions once created are kept.
}
