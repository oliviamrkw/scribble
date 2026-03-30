#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include "net.h"

#define BACKLOG 5

#ifndef PORT
#define PORT DEFAULT_PORT
#endif

typedef struct {
    int fd;
    char name[MAX_NAME_LEN];
    int active;
} client_info_t;

static client_info_t clients[MAX_PLAYERS];
static int listen_fd = -1;

static void init_clients(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        clients[i].fd = -1;
        clients[i].name[0] = '\0';
        clients[i].active = 0;
    }
}

/*
 * Find an empty slot in the clients array.
 * Returns the index, or -1 if the server is full.
 */
static int find_empty_slot(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd == -1)
            return i;
    }
    return -1;
}

/*
 * Accept a new client connection and store it in the clients array.
 * Returns the slot index on success, -1 if full or on error.
 */
static int accept_client(void)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }

    int slot = find_empty_slot();
    if (slot < 0) {
        fprintf(stderr, "Server full, rejecting %s:%d\n",
                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        close(client_fd);
        return -1;
    }

    clients[slot].fd = client_fd;
    clients[slot].active = 0;
    clients[slot].name[0] = '\0';

    printf("New connection from %s:%d (slot %d, fd %d)\n",
           inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
           slot, client_fd);
    return slot;
}

/*
 * Remove a client: close socket, clear slot, notify others.
 */
static void remove_client(int slot)
{
    if (slot < 0 || slot >= MAX_PLAYERS || clients[slot].fd == -1)
        return;

    printf("Removing client \"%s\" (slot %d, fd %d)\n",
           clients[slot].name, slot, clients[slot].fd);

    close(clients[slot].fd);
    clients[slot].fd = -1;
    clients[slot].active = 0;
    clients[slot].name[0] = '\0';

    /* Count remaining active players */
    int remaining = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1)
            remaining++;
    }

    /* Broadcast PLAYER_DISCONNECT to remaining clients */
    message_t disc;
    memset(&disc, 0, sizeof(disc));
    disc.msg_type = MSG_PLAYER_DISCONNECT;
    uint32_t net_id = htonl((uint32_t)slot);
    uint32_t net_rem = htonl((uint32_t)remaining);
    memcpy(disc.payload, &net_id, sizeof(net_id));
    memcpy(disc.payload + sizeof(net_id), &net_rem, sizeof(net_rem));
    disc.length = sizeof(net_id) + sizeof(net_rem);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1) {
            send_message(clients[i].fd, &disc);
        }
    }
}

/*
 * Send a message to all connected clients (optionally skip one slot).
 * If skip_slot is -1, send to everyone.
 */
static void broadcast_to_clients(const message_t *msg, int skip_slot)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1 && i != skip_slot) {
            if (send_message(clients[i].fd, msg) < 0) {
                fprintf(stderr, "Failed to send to slot %d, removing\n", i);
                remove_client(i);
            }
        }
    }
}

/*
 * Handle a PLAYER_JOIN message from a newly connected client.
 */
static void handle_player_join(int slot, const message_t *msg)
{
    if (msg->length < sizeof(uint32_t) + 1) {
        fprintf(stderr, "Malformed PLAYER_JOIN from slot %d\n", slot);
        remove_client(slot);
        return;
    }

    uint32_t name_len;
    memcpy(&name_len, msg->payload, sizeof(name_len));
    name_len = ntohl(name_len);

    if (name_len == 0 || name_len >= MAX_NAME_LEN) {
        fprintf(stderr, "Invalid name length %u from slot %d\n",
                name_len, slot);
        remove_client(slot);
        return;
    }

    memcpy(clients[slot].name, msg->payload + sizeof(uint32_t), name_len);
    clients[slot].name[name_len] = '\0';
    clients[slot].active = 1;

    printf("Player \"%s\" joined (slot %d)\n", clients[slot].name, slot);
}

/*
 * Handle an incoming message from a connected client.
 */
static void handle_client_message(int slot)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    if (recv_message(clients[slot].fd, &msg) < 0) {
        remove_client(slot);
        return;
    }

    switch (msg.msg_type) {
    case MSG_PLAYER_JOIN:
        handle_player_join(slot, &msg);
        break;

    case MSG_BRUSH_STROKE:
        /* Artist sends stroke -> broadcast to all other clients */
        broadcast_to_clients(&msg, slot);
        break;

    case MSG_GUESS:
        /*
         * TODO: integrate with game logic (Person B's game.c)
         * For now, just forward to all other clients so they can see
         * guesses in the chat.
         */
        broadcast_to_clients(&msg, slot);
        break;

    default:
        fprintf(stderr, "Unknown message type %u from slot %d\n",
                msg.msg_type, slot);
        break;
    }
}

/*
 * Build the fd_set for select(), return the highest fd.
 */
static int build_fdset(fd_set *readfds)
{
    FD_ZERO(readfds);
    FD_SET(listen_fd, readfds);
    int maxfd = listen_fd;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1) {
            FD_SET(clients[i].fd, readfds);
            if (clients[i].fd > maxfd)
                maxfd = clients[i].fd;
        }
    }
    return maxfd;
}

static int setup_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char *argv[])
{
    int port = PORT;
    if (argc > 1)
        port = atoi(argv[1]);

    init_clients();

    listen_fd = setup_server(port);
    if (listen_fd < 0)
        exit(EXIT_FAILURE);

    printf("Server listening on port %d\n", port);

    /* Main select() loop */
    while (1) {
        fd_set readfds;
        int maxfd = build_fdset(&readfds);

        struct timeval tv;
        tv.tv_sec = ROUND_TIME_SEC;
        tv.tv_usec = 0;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (activity == 0) {
            /* Timeout — will be used for round timer in later phases */
            continue;
        }

        /* Check for new incoming connections */
        if (FD_ISSET(listen_fd, &readfds)) {
            accept_client();
        }

        /* Check each connected client for incoming data */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (clients[i].fd != -1 &&
                FD_ISSET(clients[i].fd, &readfds)) {
                handle_client_message(i);
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1)
            close(clients[i].fd);
    }
    close(listen_fd);
    return 0;
}
