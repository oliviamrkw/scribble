#define _POSIX_C_SOURCE 200112L
#include <strings.h>
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
#include <time.h>
#include "net.h"
#include "game.h"

#define BACKLOG 5
#define WORDS_FILE "words.txt"

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
static game_state_t *game = NULL;
static time_t round_start_time = 0;
static int last_countdown = -1;  /* Last countdown value broadcast */

static void init_clients(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        clients[i].fd = -1;
        clients[i].name[0] = '\0';
        clients[i].active = 0;
    }
}

static int find_empty_slot(void)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd == -1)
            return i;
    }
    return -1;
}

static void send_chat(int fd, const char *text)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_CHAT;
    msg.length = (uint32_t)strlen(text);
    if (msg.length > MAX_PAYLOAD)
        msg.length = MAX_PAYLOAD;
    memcpy(msg.payload, text, msg.length);
    send_message(fd, &msg);
}

static void broadcast_chat(const char *text)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1)
            send_chat(clients[i].fd, text);
    }
}

static void remove_client(int slot);
static void start_round(void);

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

static int count_active_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1 && clients[i].active)
            n++;
    }
    return n;
}

/*
 * Show final scoreboard after all rounds are done.
 */
static void show_final_scoreboard(void)
{
    char buf[MAX_PAYLOAD];
    int pos = snprintf(buf, sizeof(buf),
        "\n============================\n"
        "       GAME OVER!\n"
        "============================\n\n"
        "Final Scoreboard:\n");

    /* Find the winner */
    uint32_t max_score = 0;
    for (uint32_t i = 0; i < game->num_players; i++) {
        if (game->players[i].score > max_score)
            max_score = game->players[i].score;
    }

    for (uint32_t i = 0; i < game->num_players && pos < (int)sizeof(buf) - 1; i++) {
        const char *trophy = (game->players[i].score == max_score) ? " <-- WINNER!" : "";
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  %s: %u pts%s\n",
            game->players[i].name,
            game->players[i].score,
            trophy);
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos,
        "\nThanks for playing!\n");

    broadcast_chat(buf);

    /* Reset for a new game */
    game->game_started = 0;
    game->round_num = 0;
}

/*
 * End the current round. If more rounds remain, auto-start the next one.
 */
static void end_round(const char *reason)
{
    game->round_active = 0;

    /* Artist points are awarded incrementally during the round,
     * so no end-of-round artist scoring needed. */

    /* Build round scoreboard */
    char buf[MAX_PAYLOAD];
    int pos = snprintf(buf, sizeof(buf),
        "\n=== Round %u/%u Over (%s) ===\nThe word was: %s\n\nScoreboard:\n",
        game->round_num, game->total_rounds, reason, game->secret_word);

    for (uint32_t i = 0; i < game->num_players && pos < (int)sizeof(buf) - 1; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  %s: %u pts%s\n",
            game->players[i].name,
            game->players[i].score,
            game->players[i].is_artist ? " (drawer)" : "");
    }

    broadcast_chat(buf);

    /* Check if game is over */
    if (game_is_over(game)) {
        show_final_scoreboard();
    } else {
        /* Auto-start next round after a brief message */
        char next[128];
        snprintf(next, sizeof(next),
            "\nNext round starting...\n");
        broadcast_chat(next);
        start_round();
    }
}

static void start_round(void)
{
    if (game->num_players < 2) {
        broadcast_chat("Need at least 2 players to start. Waiting...\n");
        game->game_started = 0;
        return;
    }

    game_start_round(game);
    round_start_time = time(NULL);
    last_countdown = ROUND_TIME_SEC;

    char hint[MAX_NAME_LEN * 2 + 1];
    game_get_hint(game, hint, sizeof(hint));

    /* Notify each client */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd == -1 || !clients[i].active)
            continue;

        int is_artist = 0;
        for (uint32_t p = 0; p < game->num_players; p++) {
            if (game->players[p].id == (uint32_t)i) {
                is_artist = game->players[p].is_artist;
                break;
            }
        }

        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_type = MSG_ROUND_START;
        msg.payload[0] = (unsigned char)is_artist;

        if (is_artist) {
            char text[MAX_PAYLOAD - 1];
            snprintf(text, sizeof(text),
                "\n=== Round %u/%u ===\n"
                "You are the DRAWER! The word is: %.*s\n"
                "Hint shown to guessers: %.*s\n"
                "Wait for others to guess. (60 seconds)\n",
                game->round_num, game->total_rounds,
                MAX_NAME_LEN, game->secret_word,
                MAX_NAME_LEN * 2, hint);
            uint32_t tlen = (uint32_t)strlen(text);
            memcpy(msg.payload + 1, text, tlen);
            msg.length = 1 + tlen;
        } else {
            char text[MAX_PAYLOAD - 1];
            snprintf(text, sizeof(text),
                "\n=== Round %u/%u ===\n"
                "Guess the word! (%u letters): %.*s\n"
                "Type your guess below. (60 seconds)\n",
                game->round_num, game->total_rounds,
                (uint32_t)strlen(game->secret_word),
                MAX_NAME_LEN * 2, hint);
            uint32_t tlen = (uint32_t)strlen(text);
            memcpy(msg.payload + 1, text, tlen);
            msg.length = 1 + tlen;
        }

        send_message(clients[i].fd, &msg);
    }
}

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

static void remove_client(int slot)
{
    if (slot < 0 || slot >= MAX_PLAYERS || clients[slot].fd == -1)
        return;

    printf("Removing client \"%s\" (slot %d, fd %d)\n",
           clients[slot].name, slot, clients[slot].fd);

    if (clients[slot].active) {
        game_remove_player(game, (uint32_t)slot);

        char buf[128];
        snprintf(buf, sizeof(buf), "%s has disconnected.\n", clients[slot].name);
        close(clients[slot].fd);
        clients[slot].fd = -1;
        clients[slot].active = 0;
        clients[slot].name[0] = '\0';
        broadcast_chat(buf);

        if (game->round_active) {
            if (game->num_players < 2) {
                end_round("not enough players");
            } else if (game_all_guessed(game)) {
                end_round("everyone guessed it");
            }
        }
    } else {
        close(clients[slot].fd);
        clients[slot].fd = -1;
        clients[slot].active = 0;
        clients[slot].name[0] = '\0';
    }
}

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

    game_add_player(game, (uint32_t)slot, clients[slot].name);

    printf("Player \"%s\" joined (slot %d)\n", clients[slot].name, slot);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s has joined! (%d player(s) connected)\n",
             clients[slot].name, count_active_clients());
    broadcast_chat(buf);

    if (!game->game_started) {
        send_chat(clients[slot].fd,
            "Welcome! Type \"play\" to start the game.\n");
    } else if (game->round_active) {
        send_chat(clients[slot].fd,
            "A round is in progress. You'll join the next one.\n");
    }
}

static void handle_guess(int slot, const message_t *msg)
{
    char guess[MAX_NAME_LEN + 256];
    uint32_t len = msg->length;
    if (len > sizeof(guess) - 1)
        len = sizeof(guess) - 1;
    memcpy(guess, msg->payload, len);
    guess[len] = '\0';

    /* Check for "play" command when no game/round is active */
    if (!game->round_active && !game->game_started) {
        if (strcasecmp(guess, "play") == 0) {
            if (game->num_players < 2) {
                send_chat(clients[slot].fd,
                    "Need at least 2 players to start.\n");
                return;
            }
            game->game_started = 1;
            game->round_num = 0;
            game->total_rounds = game->num_players;

            /* Reset all scores for new game */
            for (uint32_t i = 0; i < game->num_players; i++)
                game->players[i].score = 0;

            char buf[128];
            snprintf(buf, sizeof(buf),
                "%s started the game! (%u rounds)\n",
                clients[slot].name, game->total_rounds);
            broadcast_chat(buf);
            start_round();
        } else {
            /* Lobby chat */
            char buf[MAX_PAYLOAD];
            snprintf(buf, sizeof(buf), "[%s] %s\n", clients[slot].name, guess);
            broadcast_chat(buf);
        }
        return;
    }

    /* Between rounds but game is started — ignore or relay as chat */
    if (!game->round_active && game->game_started) {
        return;
    }

    /* Round is active — find this player */
    player_t *player = NULL;
    for (uint32_t i = 0; i < game->num_players; i++) {
        if (game->players[i].id == (uint32_t)slot) {
            player = &game->players[i];
            break;
        }
    }

    if (!player) {
        send_chat(clients[slot].fd,
            "You're not in the current round. Wait for the next one.\n");
        return;
    }

    if (player->is_artist) {
        send_chat(clients[slot].fd,
            "You're the drawer! You can't guess.\n");
        return;
    }

    if (player->has_guessed) {
        send_chat(clients[slot].fd,
            "You already guessed correctly! Wait for the round to end.\n");
        return;
    }

    /* Check the guess */
    if (game_validate_guess(game, guess)) {
        uint32_t guesser_pts = game_get_guesser_points(game);
        uint32_t artist_pts = game_get_artist_points_for_guess(game);
        player->score += guesser_pts;

        /* Award artist points */
        for (uint32_t i = 0; i < game->num_players; i++) {
            if (game->players[i].is_artist) {
                game->players[i].score += artist_pts;
                break;
            }
        }

        game_mark_guessed(game, (uint32_t)slot);

        /* Notify the guesser privately */
        message_t notify;
        memset(&notify, 0, sizeof(notify));
        notify.msg_type = MSG_GUESSED_NOTIFY;
        char nbuf[MAX_PAYLOAD];
        snprintf(nbuf, sizeof(nbuf),
            "Correct! You earned %u points! (Total: %u)\n",
            guesser_pts, player->score);
        notify.length = (uint32_t)strlen(nbuf);
        memcpy(notify.payload, nbuf, notify.length);
        send_message(clients[slot].fd, &notify);

        /* Broadcast to everyone */
        char buf[MAX_PAYLOAD];
        snprintf(buf, sizeof(buf), "%s guessed the word! (+%u pts)\n",
                 clients[slot].name, guesser_pts);
        broadcast_chat(buf);

        if (game_all_guessed(game)) {
            end_round("everyone guessed it");
        }
    } else {
        /* Wrong guess — show to all as [Name] guess */
        char buf[MAX_PAYLOAD];
        snprintf(buf, sizeof(buf), "[%s] %s\n",
                 clients[slot].name, guess);
        broadcast_chat(buf);
    }
}

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

    case MSG_GUESS:
        handle_guess(slot, &msg);
        break;

    case MSG_BRUSH_STROKE:
        broadcast_to_clients(&msg, slot);
        break;

    default:
        fprintf(stderr, "Unknown message type %u from slot %d\n",
                msg.msg_type, slot);
        break;
    }
}

/*
 * Broadcast countdown timer updates at key moments.
 */
static void check_countdown(void)
{
    if (!game->round_active || round_start_time == 0)
        return;

    time_t elapsed = time(NULL) - round_start_time;
    int remaining = ROUND_TIME_SEC - (int)elapsed;
    if (remaining < 0)
        remaining = 0;

    /* Broadcast at these thresholds */
    int should_broadcast = 0;
    if (remaining <= 10 && remaining != last_countdown) {
        /* Every second in the last 10 */
        should_broadcast = 1;
    } else if (remaining <= 30 && remaining % 10 == 0 && remaining != last_countdown) {
        /* Every 10 seconds from 30 down */
        should_broadcast = 1;
    } else if (remaining % 15 == 0 && remaining != last_countdown && remaining < ROUND_TIME_SEC) {
        /* Every 15 seconds otherwise */
        should_broadcast = 1;
    }

    if (should_broadcast) {
        last_countdown = remaining;
        char buf[64];
        snprintf(buf, sizeof(buf), ">>> %d seconds remaining <<<\n", remaining);
        broadcast_chat(buf);
    }
}

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

    game = game_init();
    if (!game) {
        fprintf(stderr, "Failed to init game\n");
        exit(EXIT_FAILURE);
    }

    if (game_load_words(game, WORDS_FILE) < 0) {
        fprintf(stderr, "Failed to load words from %s\n", WORDS_FILE);
        game_cleanup(game);
        exit(EXIT_FAILURE);
    }

    listen_fd = setup_server(port);
    if (listen_fd < 0) {
        game_cleanup(game);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", port);

    while (1) {
        fd_set readfds;
        int maxfd = build_fdset(&readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        /* Check round timer */
        if (game->round_active && round_start_time > 0) {
            time_t elapsed = time(NULL) - round_start_time;
            if (elapsed >= ROUND_TIME_SEC) {
                end_round("time's up");
            } else {
                check_countdown();
            }
        }

        if (activity == 0)
            continue;

        if (FD_ISSET(listen_fd, &readfds)) {
            accept_client();
        }

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (clients[i].fd != -1 &&
                FD_ISSET(clients[i].fd, &readfds)) {
                handle_client_message(i);
            }
        }
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].fd != -1)
            close(clients[i].fd);
    }
    close(listen_fd);
    game_cleanup(game);
    return 0;
}
