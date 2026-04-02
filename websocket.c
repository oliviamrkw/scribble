#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include "websocket.h"

/* ------------------------------------------------------------------ */
/*  Minimal SHA-1 (RFC 3174)                                          */
/* ------------------------------------------------------------------ */

static uint32_t sha1_rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1(const uint8_t *data, size_t len, uint8_t hash[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    /* Pre-processing: pad message */
    size_t ml = len * 8;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = calloc(padded_len, 1);
    if (!msg) return;
    memcpy(msg, data, len);
    msg[len] = 0x80;
    /* Append length in bits as big-endian 64-bit */
    for (int i = 0; i < 8; i++)
        msg[padded_len - 1 - i] = (uint8_t)(ml >> (i * 8));

    /* Process each 64-byte block */
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[offset + i*4] << 24)
                  | ((uint32_t)msg[offset + i*4+1] << 16)
                  | ((uint32_t)msg[offset + i*4+2] << 8)
                  | ((uint32_t)msg[offset + i*4+3]);
        }
        for (int i = 16; i < 80; i++)
            w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t tmp = sha1_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1_rotl(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);

    uint32_t hh[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        hash[i*4]   = (uint8_t)(hh[i] >> 24);
        hash[i*4+1] = (uint8_t)(hh[i] >> 16);
        hash[i*4+2] = (uint8_t)(hh[i] >> 8);
        hash[i*4+3] = (uint8_t)(hh[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Base64 encoder                                                    */
/* ------------------------------------------------------------------ */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64chars[(in[i] >> 2) & 0x3F];
        out[j++] = b64chars[((in[i] & 0x3) << 4) | (in[i+1] >> 4)];
        out[j++] = b64chars[((in[i+1] & 0xF) << 2) | (in[i+2] >> 6)];
        out[j++] = b64chars[in[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = b64chars[(in[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = b64chars[((in[i] & 0x3) << 4) | (in[i+1] >> 4)];
            out[j++] = b64chars[(in[i+1] & 0xF) << 2];
        } else {
            out[j++] = b64chars[(in[i] & 0x3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------ */
/*  WebSocket handshake                                               */
/* ------------------------------------------------------------------ */

#define WS_GUID "258EAFA5-E914-47DA-95CA-5AB9DC11E85A"
#define HANDSHAKE_BUF 4096

int ws_do_handshake(int fd)
{
    char buf[HANDSHAKE_BUF];
    size_t total = 0;

    /* Read until we have the full HTTP request (ends with \r\n\r\n) */
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) {
            fprintf(stderr, "ws_handshake: recv failed (%zd)\n", n);
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            break;
    }

    fprintf(stderr, "ws_handshake: received %zu bytes\n", total);

    /* Find Sec-WebSocket-Key header (case-insensitive search) */
    char *pos = NULL;
    const char *variants[] = {
        "Sec-WebSocket-Key:", "Sec-Websocket-Key:",
        "sec-websocket-key:", "SEC-WEBSOCKET-KEY:", NULL
    };
    for (int v = 0; variants[v]; v++) {
        pos = strstr(buf, variants[v]);
        if (pos) {
            pos += strlen(variants[v]);
            break;
        }
    }

    if (!pos) {
        fprintf(stderr, "ws_handshake: Sec-WebSocket-Key not found\n");
        fprintf(stderr, "ws_handshake: request was:\n%s\n", buf);
        return -1;
    }

    while (*pos == ' ') pos++;

    char key[128];
    int i = 0;
    while (*pos && *pos != '\r' && *pos != '\n' && *pos != ' ' && i < 126)
        key[i++] = *pos++;
    key[i] = '\0';

    fprintf(stderr, "ws_handshake: key='%s' (%d chars)\n", key, i);

    /* Compute accept value: SHA1(key + GUID) then base64 */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

    uint8_t hash[20];
    sha1((const uint8_t *)combined, strlen(combined), hash);

    char accept[64];
    base64_encode(hash, 20, accept);

    fprintf(stderr, "ws_handshake: accept='%s'\n", accept);

    /* Send response */
    char response[512];
    int rlen = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);

    ssize_t sent = 0;
    while (sent < rlen) {
        ssize_t w = write(fd, response + sent, (size_t)(rlen - sent));
        if (w <= 0) return -1;
        sent += w;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  WebSocket frame send (server → client, no mask)                   */
/* ------------------------------------------------------------------ */

int ws_send_frame(int fd, const void *data, size_t len)
{
    uint8_t header[10];
    int hlen = 0;

    header[0] = 0x82; /* FIN=1, opcode=binary */
    if (len <= 125) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        /* Messages > 64KB not expected */
        return -1;
    }

    /* Send header then payload */
    ssize_t sent = 0;
    while (sent < hlen) {
        ssize_t w = write(fd, header + sent, (size_t)(hlen - sent));
        if (w <= 0) return -1;
        sent += w;
    }
    sent = 0;
    while ((size_t)sent < len) {
        ssize_t w = write(fd, (const uint8_t *)data + sent, len - (size_t)sent);
        if (w <= 0) return -1;
        sent += w;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  WebSocket frame recv (client → server, masked)                    */
/* ------------------------------------------------------------------ */

/* Helper: read exactly n bytes */
static int ws_read_all(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (uint8_t *)buf + total, n - total);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

int ws_recv_frame(int fd, void *buf, size_t bufsize, uint8_t *opcode)
{
    uint8_t header[2];
    if (ws_read_all(fd, header, 2) < 0) return -1;

    *opcode = header[0] & 0x0F;
    int masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (ws_read_all(fd, ext, 2) < 0) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (ws_read_all(fd, ext, 8) < 0) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (ws_read_all(fd, mask, 4) < 0) return -1;
    }

    if (payload_len > bufsize) return -1;

    if (payload_len > 0) {
        if (ws_read_all(fd, buf, (size_t)payload_len) < 0) return -1;
        if (masked) {
            uint8_t *p = (uint8_t *)buf;
            for (uint64_t i = 0; i < payload_len; i++)
                p[i] ^= mask[i % 4];
        }
    }

    /* Handle control frames */
    if (*opcode == WS_OP_CLOSE) return 0;
    if (*opcode == WS_OP_PING) {
        /* Send pong */
        uint8_t pong_hdr[2] = {0x8A, (uint8_t)(payload_len & 0x7F)};
        write(fd, pong_hdr, 2);
        if (payload_len > 0) write(fd, buf, (size_t)payload_len);
        /* Recursive: get next real frame */
        return ws_recv_frame(fd, buf, bufsize, opcode);
    }

    return (int)payload_len;
}
