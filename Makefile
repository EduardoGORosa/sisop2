CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

COMMON_OBJS = common/packet.o

CLIENT_SRCS = client/client.c client/client_actions.c client/client_sync.c
# CLIENT_OBJS lists all object files needed for the client executable
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o) $(COMMON_OBJS)
CLIENT_EXEC = myClient

SERVER_SRCS = server/server.c server/server_session.c server/server_request_handler.c server/server_utils.c
# SERVER_OBJS lists all object files needed for the server executable
SERVER_OBJS = $(SERVER_SRCS:.c=.o) $(COMMON_OBJS)
SERVER_EXEC = myServer

# Default target: build both client and server
all: $(CLIENT_EXEC) $(SERVER_EXEC)

# Rule to link the client executable
$(CLIENT_EXEC): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to link the server executable
$(SERVER_EXEC): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Generic rule to compile .c files into .o files (will be overridden by more specific rules below)
# %.o: %.c
#	$(CC) $(CFLAGS) -c $< -o $@

# Specific rules for compiling .c files from subdirectories into .o files in those same subdirectories
common/%.o: common/%.c ../common/packet.h
	$(CC) $(CFLAGS) -c $< -o $@

client/%.o: client/%.c ../common/packet.h client/client_actions.h client/client_sync.h
	$(CC) $(CFLAGS) -c $< -o $@

server/%.o: server/%.c ../common/packet.h server/server_session.h server/server_request_handler.h server/server_utils.h
	$(CC) $(CFLAGS) -c $< -o $@

# Clean rule to remove compiled files
clean:
	rm -f $(CLIENT_EXEC) $(SERVER_EXEC) \
	      client/*.o server/*.o common/*.o \
	      core.* *~
