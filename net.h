#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

#define DEFAULT_PORT    4242
#define MAX_PAYLOAD     1024
#define MAX_PLAYERS     6
#define MAX_NAME_LEN    32
#define ROUND_TIME_SEC  60
#define CANVAS_COLS     40
#define CANVAS_ROWS     20
#define NUM_COLORS      8

/* Message type IDs */
#define MSG_PLAYER_JOIN       1
#define MSG_ROUND_START       2
#define MSG_DRAW_CELL         3
#define MSG_GUESS             4
#define MSG_CORRECT_GUESS     5
#define MSG_ROUND_END         6
#define MSG_PLAYER_DISCONNECT 7
#define MSG_CHAT              8
#define MSG_GUESSED_NOTIFY    9
#define MSG_CANVAS_SYNC      10
#define MSG_DRAW_CLEAR       11

/* Wire format (network byte order):
[4 bytes msg_type] [4 bytes payload length] [length bytes payload] */

typedef struct {
    uint32_t msg_type;
    uint32_t length;
    unsigned char payload[MAX_PAYLOAD];
} message_t;

/* Send a complete message over a connected socket */
int send_message(int sockfd, const message_t *msg);

/* Receive a complete message from a connected socket */
int recv_message(int sockfd, message_t *msg);

/* Write exactly n bytes to fd, handling partial writes */
int write_all(int fd, const void *buf, size_t n);

/* Read exactly n bytes from fd, handling partial reads */
int read_all(int fd, void *buf, size_t n);

#endif
