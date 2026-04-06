#define _POSIX_C_SOURCE 200112L
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include "net.h"
#include "game.h"

#define BACKLOG          5
#define WORDS_FILE       "words.txt"
#define IDLE_WARN_SEC    90
#define IDLE_KICK_SEC    120
#define HINT_REVEALS     3

#ifndef PORT
#define PORT DEFAULT_PORT
#endif

/* ------------------------------------------------------------------ */
/*  Logging                                                           */
/* ------------------------------------------------------------------ */

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

static log_level_t current_log_level = LOG_INFO;

static void server_log(log_level_t lvl, const char *fmt, ...)
{
    if (lvl < current_log_level)
        return;

    static const char *tags[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    fprintf(stderr, "[%02d:%02d:%02d][%s] ",
            t->tm_hour, t->tm_min, t->tm_sec, tags[lvl]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/*  Signal handling  (must be file-scope for the handler)             */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t got_sigint = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    got_sigint = 1;
}

/* ------------------------------------------------------------------ */
/*  Data types                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int    fd;
    char   name[MAX_NAME_LEN];
    int    active;
    time_t last_active;
    int    idle_warned;
} client_info_t;

typedef struct {
    client_info_t  clients[MAX_PLAYERS];
    int            listen_fd;
    game_state_t  *game;

    /* Round timer */
    time_t         round_start_time;
    int            last_countdown;

    /* Progressive hints */
    int            revealed[MAX_NAME_LEN];
    int            revealed_count;
    int            next_reveal_threshold;
    char           current_hint[MAX_NAME_LEN * 2 + 1];

    /* Lifetime statistics */
    time_t         start_time;
    int            total_connections;
    int            total_rounds;
} server_ctx_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

static void remove_client(server_ctx_t *ctx, int slot);
static void start_round(server_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/*  Client helpers                                                    */
/* ------------------------------------------------------------------ */

static void init_clients(server_ctx_t *ctx)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        ctx->clients[i].fd          = -1;
        ctx->clients[i].name[0]     = '\0';
        ctx->clients[i].active      = 0;
        ctx->clients[i].last_active = 0;
        ctx->clients[i].idle_warned = 0;
    }
}

static int find_empty_slot(server_ctx_t *ctx)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd == -1)
            return i;
    }
    return -1;
}

static int count_active_clients(server_ctx_t *ctx)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd != -1 && ctx->clients[i].active)
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Messaging                                                         */
/* ------------------------------------------------------------------ */

static int send_chat(int fd, const char *text)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_CHAT;
    msg.length = (uint32_t)strlen(text);
    if (msg.length > MAX_PAYLOAD)
        msg.length = MAX_PAYLOAD;
    memcpy(msg.payload, text, msg.length);
    return send_message(fd, &msg);
}

static void broadcast_chat(server_ctx_t *ctx, const char *text)
{
    int failed[MAX_PLAYERS];
    int nfailed = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd != -1) {
            if (send_chat(ctx->clients[i].fd, text) < 0)
                failed[nfailed++] = i;
        }
    }

    for (int i = 0; i < nfailed; i++)
        remove_client(ctx, failed[i]);
}

static void broadcast_to_clients(server_ctx_t *ctx, const message_t *msg,
                                  int skip_slot)
{
    int failed[MAX_PLAYERS];
    int nfailed = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd != -1 && i != skip_slot) {
            if (send_message(ctx->clients[i].fd, msg) < 0)
                failed[nfailed++] = i;
        }
    }

    for (int i = 0; i < nfailed; i++) {
        server_log(LOG_WARN, "Send failed for slot %d, removing", failed[i]);
        remove_client(ctx, failed[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Scoreboard                                                        */
/* ------------------------------------------------------------------ */

static void show_final_scoreboard(server_ctx_t *ctx)
{
    char buf[MAX_PAYLOAD];
    int pos = snprintf(buf, sizeof(buf),
        "\n============================\n"
        "       GAME OVER!\n"
        "============================\n\n"
        "Final Scoreboard:\n");

    uint32_t max_score = 0;
    for (uint32_t i = 0; i < ctx->game->num_players; i++) {
        if (ctx->game->players[i].score > max_score)
            max_score = ctx->game->players[i].score;
    }

    for (uint32_t i = 0; i < ctx->game->num_players &&
         pos < (int)sizeof(buf) - 1; i++) {
        const char *trophy =
            (ctx->game->players[i].score == max_score) ? " <-- WINNER!" : "";
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  %s: %u pts%s\n",
            ctx->game->players[i].name,
            ctx->game->players[i].score,
            trophy);
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\nThanks for playing!\n");

    broadcast_chat(ctx, buf);

    ctx->game->game_started = 0;
    ctx->game->round_num = 0;
}

/* ------------------------------------------------------------------ */
/*  Round management                                                  */
/* ------------------------------------------------------------------ */

static void end_round(server_ctx_t *ctx, const char *reason)
{
    ctx->game->round_active = 0;
    ctx->total_rounds++;

    {
        message_t re;
        memset(&re, 0, sizeof(re));
        re.msg_type = MSG_ROUND_END;
        uint32_t net_rn = htonl(ctx->game->round_num);
        uint32_t word_len = (uint32_t)strlen(ctx->game->secret_word);
        uint32_t net_wl = htonl(word_len);
        memcpy(re.payload, &net_rn, sizeof(net_rn));
        memcpy(re.payload + sizeof(net_rn), &net_wl, sizeof(net_wl));
        memcpy(re.payload + 2 * sizeof(uint32_t),
               ctx->game->secret_word, word_len);
        re.length = 2 * sizeof(uint32_t) + word_len;
        broadcast_to_clients(ctx, &re, -1);
    }

    char buf[MAX_PAYLOAD];
    int pos = snprintf(buf, sizeof(buf),
        "\n=== Round %u/%u Over (%s) ===\nThe word was: %s\n\nScoreboard:\n",
        ctx->game->round_num, ctx->game->total_rounds,
        reason, ctx->game->secret_word);

    for (uint32_t i = 0; i < ctx->game->num_players &&
         pos < (int)sizeof(buf) - 1; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  %s: %u pts%s\n",
            ctx->game->players[i].name,
            ctx->game->players[i].score,
            ctx->game->players[i].is_artist ? " (drawer)" : "");
    }

    broadcast_chat(ctx, buf);

    server_log(LOG_INFO, "Round %u/%u ended: %s",
               ctx->game->round_num, ctx->game->total_rounds, reason);

    if (game_is_over(ctx->game)) {
        show_final_scoreboard(ctx);
    } else {
        broadcast_chat(ctx, "\nNext round starting...\n");
        start_round(ctx);
    }
}

/* ------------------------------------------------------------------ */
/*  Progressive hints                                                 */
/* ------------------------------------------------------------------ */

static void build_current_hint(server_ctx_t *ctx)
{
    const char *word = ctx->game->secret_word;
    char *out = ctx->current_hint;
    size_t pos = 0;
    size_t cap = sizeof(ctx->current_hint);

    for (size_t i = 0; word[i] && pos + 2 < cap; i++) {
        if (word[i] == ' ') {
            out[pos++] = ' ';
            out[pos++] = ' ';
        } else {
            if (i > 0)
                out[pos++] = ' ';
            out[pos++] = ctx->revealed[i] ? word[i] : '_';
        }
    }
    out[pos] = '\0';
}

static void init_round_hints(server_ctx_t *ctx)
{
    memset(ctx->revealed, 0, sizeof(ctx->revealed));
    ctx->revealed_count = 0;
    ctx->next_reveal_threshold = ROUND_TIME_SEC * 3 / 4;
    build_current_hint(ctx);
}

static void check_hint_reveal(server_ctx_t *ctx)
{
    if (!ctx->game->round_active || ctx->round_start_time == 0)
        return;
    if (ctx->revealed_count >= HINT_REVEALS)
        return;

    time_t elapsed = time(NULL) - ctx->round_start_time;
    int remaining = ROUND_TIME_SEC - (int)elapsed;

    if (remaining > ctx->next_reveal_threshold)
        return;

    const char *word = ctx->game->secret_word;
    size_t wlen = strlen(word);

    int hidden[MAX_NAME_LEN];
    int nhidden = 0;
    for (size_t i = 0; i < wlen; i++) {
        if (word[i] != ' ' && !ctx->revealed[i])
            hidden[nhidden++] = (int)i;
    }
    if (nhidden == 0)
        return;

    int pick = rand() % nhidden;
    ctx->revealed[hidden[pick]] = 1;
    ctx->revealed_count++;

    if (ctx->revealed_count == 1)
        ctx->next_reveal_threshold = ROUND_TIME_SEC / 2;
    else if (ctx->revealed_count == 2)
        ctx->next_reveal_threshold = ROUND_TIME_SEC / 4;
    else
        ctx->next_reveal_threshold = -1;

    build_current_hint(ctx);

    char buf[MAX_PAYLOAD];
    snprintf(buf, sizeof(buf), "HINT: %s\n", ctx->current_hint);
    broadcast_chat(ctx, buf);

    server_log(LOG_DEBUG, "Revealed letter at position %d -> \"%s\"",
               hidden[pick], ctx->current_hint);
}

/* ------------------------------------------------------------------ */
/*  start_round                                                       */
/* ------------------------------------------------------------------ */

static void start_round(server_ctx_t *ctx)
{
    if (ctx->game->num_players < 2) {
        broadcast_chat(ctx,
            "Need at least 2 players to start. Waiting...\n");
        ctx->game->game_started = 0;
        return;
    }

    game_start_round(ctx->game);
    ctx->round_start_time = time(NULL);
    ctx->last_countdown = ROUND_TIME_SEC;

    init_round_hints(ctx);

    server_log(LOG_INFO, "Round %u/%u started (artist=%u, word=\"%s\")",
               ctx->game->round_num, ctx->game->total_rounds,
               ctx->game->artist_id, ctx->game->secret_word);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd == -1 || !ctx->clients[i].active)
            continue;

        int is_artist = 0;
        for (uint32_t p = 0; p < ctx->game->num_players; p++) {
            if (ctx->game->players[p].id == (uint32_t)i) {
                is_artist = ctx->game->players[p].is_artist;
                break;
            }
        }

        message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_type = MSG_ROUND_START;
        msg.payload[0] = (unsigned char)is_artist;

        char text[MAX_PAYLOAD - 1];
        if (is_artist) {
            snprintf(text, sizeof(text),
                "\n=== Round %u/%u ===\n"
                "You are the DRAWER! The word is: %.*s\n"
                "Hint shown to guessers: %.*s\n"
                "Wait for others to guess. (%d seconds)\n",
                ctx->game->round_num, ctx->game->total_rounds,
                MAX_NAME_LEN, ctx->game->secret_word,
                (int)sizeof(ctx->current_hint), ctx->current_hint,
                ROUND_TIME_SEC);
        } else {
            snprintf(text, sizeof(text),
                "\n=== Round %u/%u ===\n"
                "Guess the word! (%u letters): %.*s\n"
                "Type your guess below. (%d seconds)\n",
                ctx->game->round_num, ctx->game->total_rounds,
                (uint32_t)strlen(ctx->game->secret_word),
                (int)sizeof(ctx->current_hint), ctx->current_hint,
                ROUND_TIME_SEC);
        }

        uint32_t tlen = (uint32_t)strlen(text);
        memcpy(msg.payload + 1, text, tlen);
        msg.length = 1 + tlen;

        send_message(ctx->clients[i].fd, &msg);
    }
}

/* ------------------------------------------------------------------ */
/*  Connection management                                             */
/* ------------------------------------------------------------------ */

static int accept_client(server_ctx_t *ctx)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int fd = accept(ctx->listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) {
        if (errno != EINTR)
            server_log(LOG_ERROR, "accept: %s", strerror(errno));
        return -1;
    }

    int slot = find_empty_slot(ctx);
    if (slot < 0) {
        server_log(LOG_WARN, "Server full, rejecting %s:%d",
                   inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        close(fd);
        return -1;
    }

    ctx->clients[slot].fd          = fd;
    ctx->clients[slot].active      = 0;
    ctx->clients[slot].name[0]     = '\0';
    ctx->clients[slot].last_active = time(NULL);
    ctx->clients[slot].idle_warned = 0;
    ctx->total_connections++;

    server_log(LOG_INFO, "Connection from %s:%d (slot %d, fd %d)",
               inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), slot, fd);
    return slot;
}

static void remove_client(server_ctx_t *ctx, int slot)
{
    if (slot < 0 || slot >= MAX_PLAYERS || ctx->clients[slot].fd == -1)
        return;

    server_log(LOG_INFO, "Removing client \"%s\" (slot %d, fd %d)",
               ctx->clients[slot].name, slot, ctx->clients[slot].fd);

    close(ctx->clients[slot].fd);
    ctx->clients[slot].fd = -1;

    if (ctx->clients[slot].active) {
        game_remove_player(ctx->game, (uint32_t)slot);

        int remaining = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i != slot && ctx->clients[i].fd != -1 &&
                ctx->clients[i].active)
                remaining++;
        }

        message_t disc;
        memset(&disc, 0, sizeof(disc));
        disc.msg_type = MSG_PLAYER_DISCONNECT;
        uint32_t net_id  = htonl((uint32_t)slot);
        uint32_t net_rem = htonl((uint32_t)remaining);
        memcpy(disc.payload, &net_id, sizeof(net_id));
        memcpy(disc.payload + sizeof(net_id), &net_rem, sizeof(net_rem));
        disc.length = sizeof(net_id) + sizeof(net_rem);
        broadcast_to_clients(ctx, &disc, slot);

        char buf[128];
        snprintf(buf, sizeof(buf), "%s has disconnected.\n",
                 ctx->clients[slot].name);
        ctx->clients[slot].active  = 0;
        ctx->clients[slot].name[0] = '\0';
        broadcast_chat(ctx, buf);

        if (ctx->game->round_active) {
            if (ctx->game->num_players < 2)
                end_round(ctx, "not enough players");
            else if (game_all_guessed(ctx->game))
                end_round(ctx, "everyone guessed it");
        }
    } else {
        ctx->clients[slot].active  = 0;
        ctx->clients[slot].name[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/*  Message handlers                                                  */
/* ------------------------------------------------------------------ */

static void handle_player_join(server_ctx_t *ctx, int slot,
                                const message_t *msg)
{
    if (msg->length < sizeof(uint32_t) + 1) {
        server_log(LOG_WARN, "Malformed PLAYER_JOIN from slot %d", slot);
        remove_client(ctx, slot);
        return;
    }

    uint32_t name_len;
    memcpy(&name_len, msg->payload, sizeof(name_len));
    name_len = ntohl(name_len);

    if (name_len == 0 || name_len >= MAX_NAME_LEN) {
        server_log(LOG_WARN, "Invalid name length %u from slot %d",
                   name_len, slot);
        remove_client(ctx, slot);
        return;
    }

    char requested[MAX_NAME_LEN];
    memcpy(requested, msg->payload + sizeof(uint32_t), name_len);
    requested[name_len] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != slot && ctx->clients[i].fd != -1 &&
            ctx->clients[i].active &&
            strcasecmp(ctx->clients[i].name, requested) == 0) {
            server_log(LOG_WARN,
                "Duplicate name \"%s\" rejected (slot %d)", requested, slot);
            send_chat(ctx->clients[slot].fd,
                "Name already taken. Reconnect with a different name.\n");
            remove_client(ctx, slot);
            return;
        }
    }

    memcpy(ctx->clients[slot].name, requested, name_len);
    ctx->clients[slot].name[name_len] = '\0';
    ctx->clients[slot].active = 1;

    game_add_player(ctx->game, (uint32_t)slot, ctx->clients[slot].name);

    server_log(LOG_INFO, "Player \"%s\" joined (slot %d)",
               ctx->clients[slot].name, slot);

    char buf[256];
    snprintf(buf, sizeof(buf), "%s has joined! (%d player(s) connected)\n",
             ctx->clients[slot].name, count_active_clients(ctx));
    broadcast_chat(ctx, buf);

    if (!ctx->game->game_started) {
        send_chat(ctx->clients[slot].fd,
            "Welcome! Type \"play\" to start the game.\n");
    } else if (ctx->game->round_active) {
        send_chat(ctx->clients[slot].fd,
            "A round is in progress. You'll join the next one.\n");
    }
}

static void handle_guess(server_ctx_t *ctx, int slot, const message_t *msg)
{
    char guess[MAX_NAME_LEN + 256];
    uint32_t len = msg->length;
    if (len > sizeof(guess) - 1)
        len = sizeof(guess) - 1;
    memcpy(guess, msg->payload, len);
    guess[len] = '\0';

    if (!ctx->game->round_active && !ctx->game->game_started) {
        if (strcasecmp(guess, "play") == 0) {
            if (ctx->game->num_players < 2) {
                send_chat(ctx->clients[slot].fd,
                    "Need at least 2 players to start.\n");
                return;
            }
            ctx->game->game_started  = 1;
            ctx->game->round_num     = 0;
            ctx->game->total_rounds  = ctx->game->num_players;

            for (uint32_t i = 0; i < ctx->game->num_players; i++)
                ctx->game->players[i].score = 0;

            char buf[128];
            snprintf(buf, sizeof(buf), "%s started the game! (%u rounds)\n",
                     ctx->clients[slot].name, ctx->game->total_rounds);
            broadcast_chat(ctx, buf);
            start_round(ctx);
        } else {
            char buf[MAX_PAYLOAD];
            snprintf(buf, sizeof(buf), "[%s] %s\n",
                     ctx->clients[slot].name, guess);
            broadcast_chat(ctx, buf);
        }
        return;
    }

    if (!ctx->game->round_active && ctx->game->game_started)
        return;

    player_t *player = NULL;
    for (uint32_t i = 0; i < ctx->game->num_players; i++) {
        if (ctx->game->players[i].id == (uint32_t)slot) {
            player = &ctx->game->players[i];
            break;
        }
    }

    if (!player) {
        send_chat(ctx->clients[slot].fd,
            "You're not in the current round. Wait for the next one.\n");
        return;
    }

    if (player->is_artist) {
        send_chat(ctx->clients[slot].fd,
            "You're the drawer! You can't guess.\n");
        return;
    }

    if (player->has_guessed) {
        send_chat(ctx->clients[slot].fd,
            "You already guessed correctly! Wait for the round to end.\n");
        return;
    }

    if (game_validate_guess(ctx->game, guess)) {
        uint32_t guesser_pts = game_get_guesser_points(ctx->game);
        uint32_t artist_pts  = game_get_artist_points_for_guess(ctx->game);
        player->score += guesser_pts;

        for (uint32_t i = 0; i < ctx->game->num_players; i++) {
            if (ctx->game->players[i].is_artist) {
                ctx->game->players[i].score += artist_pts;
                break;
            }
        }

        game_mark_guessed(ctx->game, (uint32_t)slot);

        {
            message_t notify;
            memset(&notify, 0, sizeof(notify));
            notify.msg_type = MSG_GUESSED_NOTIFY;
            char nbuf[MAX_PAYLOAD];
            snprintf(nbuf, sizeof(nbuf),
                "Correct! You earned %u points! (Total: %u)\n",
                guesser_pts, player->score);
            notify.length = (uint32_t)strlen(nbuf);
            memcpy(notify.payload, nbuf, notify.length);
            send_message(ctx->clients[slot].fd, &notify);
        }

        {
            message_t cg;
            memset(&cg, 0, sizeof(cg));
            cg.msg_type = MSG_CORRECT_GUESS;
            uint32_t net_gid  = htonl((uint32_t)slot);
            uint32_t net_gpts = htonl(guesser_pts);
            uint32_t net_apts = htonl(artist_pts);
            memcpy(cg.payload, &net_gid, sizeof(net_gid));
            memcpy(cg.payload + sizeof(net_gid),
                   &net_gpts, sizeof(net_gpts));
            memcpy(cg.payload + sizeof(net_gid) + sizeof(net_gpts),
                   &net_apts, sizeof(net_apts));
            cg.length = 3 * sizeof(uint32_t);
            broadcast_to_clients(ctx, &cg, -1);
        }

        char buf[MAX_PAYLOAD];
        snprintf(buf, sizeof(buf), "%s guessed the word! (+%u pts)\n",
                 ctx->clients[slot].name, guesser_pts);
        broadcast_chat(ctx, buf);

        if (game_all_guessed(ctx->game))
            end_round(ctx, "everyone guessed it");
    } else {
        char buf[MAX_PAYLOAD];
        snprintf(buf, sizeof(buf), "[%s] %s\n",
                 ctx->clients[slot].name, guess);
        broadcast_chat(ctx, buf);
    }
}

static void handle_client_message(server_ctx_t *ctx, int slot)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    if (recv_message(ctx->clients[slot].fd, &msg) < 0) {
        remove_client(ctx, slot);
        return;
    }

    ctx->clients[slot].last_active = time(NULL);
    ctx->clients[slot].idle_warned = 0;

    switch (msg.msg_type) {
    case MSG_PLAYER_JOIN:
        handle_player_join(ctx, slot, &msg);
        break;
    case MSG_GUESS:
        handle_guess(ctx, slot, &msg);
        break;
    case MSG_BRUSH_STROKE:
        broadcast_to_clients(ctx, &msg, slot);
        break;
    default:
        server_log(LOG_WARN, "Unknown msg type %u from slot %d",
                   msg.msg_type, slot);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Timer-based checks                                                */
/* ------------------------------------------------------------------ */

static void check_countdown(server_ctx_t *ctx)
{
    if (!ctx->game->round_active || ctx->round_start_time == 0)
        return;

    time_t elapsed = time(NULL) - ctx->round_start_time;
    int remaining = ROUND_TIME_SEC - (int)elapsed;
    if (remaining < 0)
        remaining = 0;

    int should = 0;
    if (remaining <= 10 && remaining != ctx->last_countdown)
        should = 1;
    else if (remaining <= 30 && remaining % 10 == 0 &&
             remaining != ctx->last_countdown)
        should = 1;
    else if (remaining % 15 == 0 && remaining != ctx->last_countdown &&
             remaining < ROUND_TIME_SEC)
        should = 1;

    if (should) {
        ctx->last_countdown = remaining;
        char buf[64];
        snprintf(buf, sizeof(buf),
                 ">>> %d seconds remaining <<<\n", remaining);
        broadcast_chat(ctx, buf);
    }
}

static void check_idle_clients(server_ctx_t *ctx)
{
    if (!ctx->game->game_started)
        return;

    time_t now = time(NULL);
    int kick[MAX_PLAYERS];
    int nkick = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd == -1 || !ctx->clients[i].active)
            continue;

        time_t idle = now - ctx->clients[i].last_active;

        if (idle >= IDLE_KICK_SEC) {
            send_chat(ctx->clients[i].fd,
                "Disconnected for inactivity.\n");
            kick[nkick++] = i;
        } else if (idle >= IDLE_WARN_SEC && !ctx->clients[i].idle_warned) {
            send_chat(ctx->clients[i].fd,
                "Warning: you will be disconnected in 30 seconds"
                " if idle.\n");
            ctx->clients[i].idle_warned = 1;
        }
    }

    for (int i = 0; i < nkick; i++) {
        server_log(LOG_INFO, "Idle kick: \"%s\" (slot %d)",
                   ctx->clients[kick[i]].name, kick[i]);
        remove_client(ctx, kick[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Admin console  (stdin via select)                                 */
/* ------------------------------------------------------------------ */

static void handle_admin_input(server_ctx_t *ctx)
{
    char line[256];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        got_sigint = 1;
        return;
    }

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        line[--len] = '\0';
    if (len == 0)
        return;

    if (strcmp(line, "/status") == 0) {
        time_t up = time(NULL) - ctx->start_time;
        server_log(LOG_INFO, "=== Server Status ===");
        server_log(LOG_INFO, "  Uptime           : %ld s", (long)up);
        server_log(LOG_INFO, "  Total connections : %d",
                   ctx->total_connections);
        server_log(LOG_INFO, "  Rounds played     : %d", ctx->total_rounds);
        server_log(LOG_INFO, "  Game started      : %s",
                   ctx->game->game_started ? "yes" : "no");
        server_log(LOG_INFO, "  Round active      : %s",
                   ctx->game->round_active ? "yes" : "no");
        if (ctx->game->round_active)
            server_log(LOG_INFO, "  Current round     : %u/%u",
                       ctx->game->round_num, ctx->game->total_rounds);

        int active = count_active_clients(ctx);
        server_log(LOG_INFO, "  Players online    : %d", active);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (ctx->clients[i].fd != -1 && ctx->clients[i].active) {
                long idle = (long)(time(NULL) - ctx->clients[i].last_active);
                server_log(LOG_INFO, "    [%d] %-16s idle %lds",
                           i, ctx->clients[i].name, idle);
            }
        }

    } else if (strncmp(line, "/kick ", 6) == 0) {
        const char *target = line + 6;
        int found = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (ctx->clients[i].fd != -1 && ctx->clients[i].active &&
                strcmp(ctx->clients[i].name, target) == 0) {
                server_log(LOG_INFO, "Admin kicked \"%s\"", target);
                send_chat(ctx->clients[i].fd,
                    "You have been kicked by the server admin.\n");
                remove_client(ctx, i);
                found = 1;
                break;
            }
        }
        if (!found)
            server_log(LOG_WARN, "Player \"%s\" not found", target);

    } else if (strcmp(line, "/endround") == 0) {
        if (ctx->game->round_active) {
            server_log(LOG_INFO, "Admin forced round end");
            end_round(ctx, "ended by admin");
        } else {
            server_log(LOG_INFO, "No active round to end");
        }

    } else if (strcmp(line, "/quit") == 0) {
        server_log(LOG_INFO, "Admin requested shutdown");
        got_sigint = 1;

    } else if (strcmp(line, "/help") == 0) {
        server_log(LOG_INFO, "Admin commands:");
        server_log(LOG_INFO, "  /status     Show server status");
        server_log(LOG_INFO, "  /kick NAME  Kick a player by name");
        server_log(LOG_INFO, "  /endround   Force-end the current round");
        server_log(LOG_INFO, "  /quit       Graceful shutdown");
        server_log(LOG_INFO, "  /help       Show this help");

    } else {
        server_log(LOG_WARN, "Unknown command: \"%s\" (try /help)", line);
    }
}

/* ------------------------------------------------------------------ */
/*  select() fd_set builder                                           */
/* ------------------------------------------------------------------ */

static int build_fdset(server_ctx_t *ctx, fd_set *readfds)
{
    FD_ZERO(readfds);

    FD_SET(STDIN_FILENO, readfds);
    int maxfd = STDIN_FILENO;

    FD_SET(ctx->listen_fd, readfds);
    if (ctx->listen_fd > maxfd)
        maxfd = ctx->listen_fd;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd != -1) {
            FD_SET(ctx->clients[i].fd, readfds);
            if (ctx->clients[i].fd > maxfd)
                maxfd = ctx->clients[i].fd;
        }
    }
    return maxfd;
}

/* ------------------------------------------------------------------ */
/*  Server socket setup                                               */
/* ------------------------------------------------------------------ */

static int setup_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        server_log(LOG_ERROR, "socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        server_log(LOG_ERROR, "setsockopt: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        server_log(LOG_ERROR, "bind: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        server_log(LOG_ERROR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Graceful shutdown                                                 */
/* ------------------------------------------------------------------ */

static void shutdown_server(server_ctx_t *ctx)
{
    server_log(LOG_INFO, "Shutting down...");

    broadcast_chat(ctx, "\nServer is shutting down. Goodbye!\n");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (ctx->clients[i].fd != -1) {
            if (ctx->clients[i].active) {
                message_t disc;
                memset(&disc, 0, sizeof(disc));
                disc.msg_type = MSG_PLAYER_DISCONNECT;
                uint32_t net_id  = htonl((uint32_t)i);
                uint32_t net_rem = htonl(0);
                memcpy(disc.payload, &net_id, sizeof(net_id));
                memcpy(disc.payload + sizeof(net_id),
                       &net_rem, sizeof(net_rem));
                disc.length = sizeof(net_id) + sizeof(net_rem);
                send_message(ctx->clients[i].fd, &disc);
            }
            close(ctx->clients[i].fd);
            ctx->clients[i].fd = -1;
        }
    }

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }

    time_t uptime = time(NULL) - ctx->start_time;
    server_log(LOG_INFO,
        "Final stats — uptime: %lds, connections: %d, rounds: %d",
        (long)uptime, ctx->total_connections, ctx->total_rounds);

    game_cleanup(ctx->game);
    ctx->game = NULL;
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int port = PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            current_log_level = LOG_DEBUG;
        else
            port = atoi(argv[i]);
    }

    server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.listen_fd      = -1;
    ctx.last_countdown = -1;
    ctx.start_time     = time(NULL);

    init_clients(&ctx);

    ctx.game = game_init();
    if (!ctx.game) {
        server_log(LOG_ERROR, "Failed to initialise game state");
        exit(EXIT_FAILURE);
    }

    if (game_load_words(ctx.game, WORDS_FILE) < 0) {
        server_log(LOG_ERROR, "Failed to load words from %s", WORDS_FILE);
        game_cleanup(ctx.game);
        exit(EXIT_FAILURE);
    }

    ctx.listen_fd = setup_server(port);
    if (ctx.listen_fd < 0) {
        game_cleanup(ctx.game);
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    server_log(LOG_INFO,
        "Listening on port %d (type /help for admin commands)", port);

    while (!got_sigint) {
        fd_set readfds;
        int maxfd = build_fdset(&ctx, &readfds);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            server_log(LOG_ERROR, "select: %s", strerror(errno));
            break;
        }

        if (ctx.game->round_active && ctx.round_start_time > 0) {
            time_t elapsed = time(NULL) - ctx.round_start_time;
            if (elapsed >= ROUND_TIME_SEC) {
                end_round(&ctx, "time's up");
            } else {
                check_countdown(&ctx);
                check_hint_reveal(&ctx);
            }
        }

        check_idle_clients(&ctx);

        if (activity == 0)
            continue;

        if (FD_ISSET(STDIN_FILENO, &readfds))
            handle_admin_input(&ctx);

        if (FD_ISSET(ctx.listen_fd, &readfds))
            accept_client(&ctx);

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (ctx.clients[i].fd != -1 &&
                FD_ISSET(ctx.clients[i].fd, &readfds)) {
                handle_client_message(&ctx, i);
            }
        }
    }

    shutdown_server(&ctx);
    return 0;
}
