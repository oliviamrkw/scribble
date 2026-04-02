#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

#define DEFAULT_PORT    4242
#define MAX_PAYLOAD     512
#define MAX_PLAYERS     6
#define MAX_NAME_LEN    32
#define ROUND_TIME_SEC  60
#define CANVAS_WIDTH    1024
#define CANVAS_HEIGHT   768

/* Message type IDs */
#define MSG_PLAYER_JOIN       1
#define MSG_ROUND_START       2
#define MSG_BRUSH_STROKE      3
#define MSG_GUESS             4
#define MSG_CORRECT_GUESS     5
#define MSG_ROUND_END         6
#define MSG_PLAYER_DISCONNECT 7
#define MSG_CHAT              8   /* Server relays a guess attempt with player name */
#define MSG_GUESSED_NOTIFY    9   /* Tell a player they guessed correctly */
#define MSG_DRAW_LINE        10   /* Line segment drawing data */
#define MSG_DRAW_CLEAR       11   /* Clear the canvas */

/*
 * Wire format (network byte order):
 *   [4 bytes msg_type] [4 bytes payload length] [length bytes payload]
 *
 * In-memory representation uses a fixed-size payload buffer to avoid
 * dynamic allocation.
 */
typedef struct {
    uint32_t msg_type;
    uint32_t length;
    unsigned char payload[MAX_PAYLOAD];
} message_t;

/*
 * Send a complete message over a connected socket.
 * Converts header fields to network byte order before transmission.
 * Returns 0 on success, -1 on error.
 */
int send_message(int sockfd, const message_t *msg);

/*
 * Receive a complete message from a connected socket.
 * Converts header fields from network byte order after reception.
 * Returns 0 on success, -1 on error or peer disconnect.
 */
int recv_message(int sockfd, message_t *msg);

/*
 * Write exactly n bytes to fd, handling partial writes.
 * Returns 0 on success, -1 on error.
 */
int write_all(int fd, const void *buf, size_t n);

/*
 * Read exactly n bytes from fd, handling partial reads.
 * Returns 0 on success, -1 on EOF or error.
 */
int read_all(int fd, void *buf, size_t n);

#endif
