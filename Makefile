PORT ?= 4242
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DPORT=$(PORT)

all: server client

server: server.o net.o game.o websocket.o
	$(CC) $(CFLAGS) -o $@ $^

client: client.o net.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c net.h game.h websocket.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o server client

.PHONY: all clean
