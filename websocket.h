#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include <stddef.h>

/* WebSocket opcodes */
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/*
 * Handle an incoming HTTP request on the WebSocket port.
 * If it's a WebSocket upgrade: perform handshake, return 0.
 * If it's a plain HTTP GET: serve the file at html_path, return 1.
 * On error: return -1.
 */
int ws_do_handshake(int fd, const char *html_path);

/*
 * Send a binary WebSocket frame (server-to-client, unmasked).
 * Returns 0 on success, -1 on error.
 */
int ws_send_frame(int fd, const void *data, size_t len);

/*
 * Receive and decode one WebSocket frame (client-to-server, masked).
 * Writes decoded payload into buf (up to bufsize bytes).
 * Sets *opcode to the frame opcode.
 * Returns payload length on success, -1 on error, 0 on close.
 */
int ws_recv_frame(int fd, void *buf, size_t bufsize, uint8_t *opcode);

#endif
