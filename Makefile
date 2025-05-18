# Makefile para dropbox_clone

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -Icommon

COMMON_SRC = common/packet.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

CLIENT_SRC = client/client.c $(COMMON_SRC)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

SERVER_SRC = server/server.c $(COMMON_SRC)
SERVER_OBJ = $(SERVER_SRC:.c=.o)

BIN_DIR = bin
CLIENT_BIN = $(BIN_DIR)/client
SERVER_BIN = $(BIN_DIR)/server

.PHONY: all clean

all: $(BIN_DIR) $(CLIENT_BIN) $(SERVER_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(CLIENT_BIN): $(CLIENT_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJ)

$(SERVER_BIN): $(SERVER_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BIN_DIR) *.o common/*.o client/*.o server/*.o

