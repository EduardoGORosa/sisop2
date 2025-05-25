#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <pthread.h>
#include "../common/packet.h" // For MAX_PAYLOAD (used for username buffer)

#define MAX_USER_LEN  MAX_PAYLOAD // Or a smaller reasonable value like 256
#define MAX_SESSIONS_PER_USER 2   // Specified in problem statement

typedef struct UserSession {
    char username[MAX_USER_LEN];
    int  active_connections_count;
    int  connection_fds[MAX_SESSIONS_PER_USER];
    struct UserSession *next;
} UserSession_t;

// Initialize session management (e.g., mutex)
void init_session_management(void);

// Get or create a user session. Returns pointer to session.
// Locks mutex. Caller must unlock.
UserSession_t *get_or_create_user_session_locked(const char *username);

// Adds a connection to a user's session.
// Assumes session_mutex is already locked.
// Returns 0 on success, -1 if session is full.
int add_connection_to_session_locked(UserSession_t *session, int conn_fd);

// Removes a connection from a user's session.
// Assumes session_mutex is already locked.
void remove_connection_from_session_locked(UserSession_t *session, int conn_fd);

// Lock and unlock session mutex (exposed for handle_client to manage scope)
void lock_sessions(void);
void unlock_sessions(void);

// Helper to find a session by username (does not lock/unlock itself)
UserSession_t* find_session_by_username_locked(const char* username);


#endif // SERVER_SESSION_H
