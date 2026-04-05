PORT ?= 4242
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -DPORT=$(PORT)

all: server client

server: server.o net.o game.o
	$(CC) $(CFLAGS) -o $@ $^

client: client.o net.o
	$(CC) $(CFLAGS) -o $@ $^ -lncurses

server.o: server.c net.h game.h
	$(CC) $(CFLAGS) -c $<

client.o: client.c net.h
	$(CC) $(CFLAGS) -c $<

net.o: net.c net.h
	$(CC) $(CFLAGS) -c $<

game.o: game.c game.h net.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o server client

.PHONY: all clean
