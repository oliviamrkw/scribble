#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "net.h"

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t written = write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        p += written;
        remaining -= (size_t)written;
    }
    return 0;
}

int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t nread = read(fd, p, remaining);
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }
        if (nread == 0) {
            /* Peer closed connection */
            return -1;
        }
        p += nread;
        remaining -= (size_t)nread;
    }
    return 0;
}

int send_message(int sockfd, const message_t *msg)
{
    if (msg->length > MAX_PAYLOAD) {
        fprintf(stderr, "send_message: payload too large (%u)\n",
                msg->length);
        return -1;
    }

    uint32_t header[2];
    header[0] = htonl(msg->msg_type);
    header[1] = htonl(msg->length);

    if (write_all(sockfd, header, sizeof(header)) < 0)
        return -1;

    if (msg->length > 0) {
        if (write_all(sockfd, msg->payload, msg->length) < 0)
            return -1;
    }
    return 0;
}

int recv_message(int sockfd, message_t *msg)
{
    uint32_t header[2];

    if (read_all(sockfd, header, sizeof(header)) < 0)
        return -1;

    msg->msg_type = ntohl(header[0]);
    msg->length = ntohl(header[1]);

    if (msg->length > MAX_PAYLOAD) {
        fprintf(stderr, "recv_message: payload too large (%u)\n",
                msg->length);
        return -1;
    }

    if (msg->length > 0) {
        if (read_all(sockfd, msg->payload, msg->length) < 0)
            return -1;
    }
    return 0;
}
