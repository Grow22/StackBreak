CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS_SERVER = -lpthread
LDFLAGS_CLIENT = -lpthread -lncursesw
LDFLAGS_LOCAL  = -lncursesw

all: server client local

server: server.c common.h
	$(CC) $(CFLAGS) -o server server.c $(LDFLAGS_SERVER)

client: client.c common.h
	$(CC) $(CFLAGS) -o client client.c $(LDFLAGS_CLIENT)

local: local.c common.h
	$(CC) $(CFLAGS) -o local local.c $(LDFLAGS_LOCAL)

clean:
	rm -f server client local highscore.dat

.PHONY: all clean
