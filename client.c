/*
 * client.c — Player client for Scribble
 *
 * Usage: ./client <host> <port>
 *
 * Connects to the server, sends a PLAYER_JOIN message with the player's
 * name, then uses select() on stdin and the server socket to send guesses
 * and receive messages.
 */
#define _POSIX_C_SOURCE 200112L
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
#include <netdb.h>
#include "net.h"

#ifndef PORT
#define PORT DEFAULT_PORT
#endif

/* Client state */
static int is_artist = 0;
static int has_guessed = 0;

/*
 * Build and send a PLAYER_JOIN message.
 * Payload: [4 bytes name_len (network order)] [name_len bytes name]
 */
static int send_player_join(int sockfd, const char *name)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_PLAYER_JOIN;

    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t net_len = htonl(name_len);
    memcpy(msg.payload, &net_len, sizeof(net_len));
    memcpy(msg.payload + sizeof(net_len), name, name_len);
    msg.length = sizeof(net_len) + name_len;

    return send_message(sockfd, &msg);
}

/*
 * Send a MSG_GUESS message containing the user's text.
 * Payload is the raw guess string (no null terminator).
 */
static int send_guess(int sockfd, const char *text, uint32_t len)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_GUESS;
    msg.length = len;
    memcpy(msg.payload, text, len);

    return send_message(sockfd, &msg);
}

/*
 * Handle a message received from the server and print it.
 */
static void handle_server_message(const message_t *msg)
{
    switch (msg->msg_type) {
    case MSG_PLAYER_JOIN:
        if (msg->length > sizeof(uint32_t)) {
            uint32_t nlen;
            memcpy(&nlen, msg->payload, sizeof(nlen));
            nlen = ntohl(nlen);
            if (nlen < MAX_NAME_LEN && nlen + sizeof(uint32_t) <= msg->length) {
                char name[MAX_NAME_LEN];
                memcpy(name, msg->payload + sizeof(uint32_t), nlen);
                name[nlen] = '\0';
                printf("[Server] Player \"%s\" joined\n", name);
            }
        }
        break;

    case MSG_ROUND_START: {
        /* payload: [1 byte is_artist] [text] */
        if (msg->length < 2)
            break;
        is_artist = msg->payload[0];
        has_guessed = 0;

        uint32_t tlen = msg->length - 1;
        if (tlen > MAX_PAYLOAD - 1)
            tlen = MAX_PAYLOAD - 1;
        char text[MAX_PAYLOAD];
        memcpy(text, msg->payload + 1, tlen);
        text[tlen] = '\0';
        printf("%s", text);
        fflush(stdout);
        break;
    }

    case MSG_BRUSH_STROKE:
        printf("[Server] Brush stroke received (%u bytes)\n", msg->length);
        break;

    case MSG_GUESS: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD)
            len = MAX_PAYLOAD;
        char buf[MAX_PAYLOAD + 1];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        printf("[Guess] %s\n", buf);
        break;
    }

    case MSG_CHAT: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD)
            len = MAX_PAYLOAD;
        char buf[MAX_PAYLOAD + 1];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        printf("%s", buf);
        fflush(stdout);
        break;
    }

    case MSG_GUESSED_NOTIFY: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD)
            len = MAX_PAYLOAD;
        char buf[MAX_PAYLOAD + 1];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        has_guessed = 1;
        printf("%s", buf);
        fflush(stdout);
        break;
    }

    case MSG_CORRECT_GUESS:
        printf("[Server] Correct guess!\n");
        break;

    case MSG_ROUND_END:
        /* Reset state for next round */
        is_artist = 0;
        has_guessed = 0;
        printf("[Server] Round ended\n");
        break;

    case MSG_PLAYER_DISCONNECT: {
        uint32_t slot_id = 0, remaining = 0;
        if (msg->length >= 2 * sizeof(uint32_t)) {
            memcpy(&slot_id, msg->payload, sizeof(slot_id));
            memcpy(&remaining, msg->payload + sizeof(slot_id),
                   sizeof(remaining));
            slot_id = ntohl(slot_id);
            remaining = ntohl(remaining);
        }
        printf("[Server] Player in slot %u disconnected (%u remaining)\n",
               slot_id, remaining);
        break;
    }

    default:
        printf("[Server] Unknown message type %u\n", msg->msg_type);
        break;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        exit(1);
    }

    /* Use getaddrinfo to resolve hostname */
    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int status = getaddrinfo(host, port_str, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        exit(1);
    }

    int sock = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, port);
        exit(1);
    }

    printf("Connected to %s:%d\n", host, port);

    int sockfd = sock;

    /* Get player name */
    char name[MAX_NAME_LEN];
    printf("Enter your name: ");
    fflush(stdout);

    if (fgets(name, sizeof(name), stdin) == NULL) {
        fprintf(stderr, "Failed to read name\n");
        close(sockfd);
        exit(1);
    }

    size_t name_len = strlen(name);
    if (name_len > 0 && name[name_len - 1] == '\n') {
        name[name_len - 1] = '\0';
        name_len--;
    }

    if (name_len == 0) {
        fprintf(stderr, "Empty name\n");
        close(sockfd);
        exit(1);
    }

    if (send_player_join(sockfd, name) < 0) {
        fprintf(stderr, "Failed to send join message\n");
        close(sockfd);
        return 1;
    }

    printf("Joined as \"%s\".\n", name);

    /* Main select() loop */
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sockfd, &readfds);

        int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        /* stdin input */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char line[MAX_PAYLOAD];
            if (fgets(line, sizeof(line), stdin) == NULL) {
                printf("Disconnecting...\n");
                break;
            }
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                line[--len] = '\0';

            if (len > 0) {
                /* Send everything to server; server enforces rules */
                if (send_guess(sockfd, line, (uint32_t)len) < 0) {
                    fprintf(stderr, "Lost connection to server\n");
                    break;
                }
            }
        }

        /* Server data */
        if (FD_ISSET(sockfd, &readfds)) {
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            if (recv_message(sockfd, &msg) < 0) {
                printf("Server disconnected\n");
                break;
            }
            handle_server_message(&msg);
        }
    }

    close(sockfd);
    return 0;
}
