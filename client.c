/*
 * client.c — ncurses terminal client for Scribble
 *
 * Usage: ./client <host> <port>
 *
 * Connects to the server, sends a PLAYER_JOIN message, then enters an
 * ncurses UI with a drawing canvas (left), chat pane (right), status bar,
 * and input line.  Uses select() on stdin and the server socket for
 * concurrent I/O.
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
#include <curses.h>
#include "net.h"

/* Layout constants — the canvas takes 2 columns per cell to look square */
#define CANVAS_DISPLAY_W (CANVAS_COLS * 2)
#define CHAT_MIN_W       24
#define STATUS_H         1
#define INPUT_H          1

/* Chat line buffer */
#define MAX_CHAT_LINES 200
#define MAX_LINE_LEN   256

/* Color names for the status bar */
static const char *color_names[NUM_COLORS] = {
    "BLACK", "RED", "GREEN", "YELLOW", "BLUE", "MAGENTA", "CYAN", "WHITE"
};

/* Client state */
static int sockfd = -1;
static int is_artist = 0;
static int has_guessed = 0;
static uint8_t canvas[CANVAS_ROWS][CANVAS_COLS];
static int cursor_row = 0, cursor_col = 0;
static int current_color = 1;  /* 1-7; 0 is eraser */
static int draw_mode = 1;      /* 1 = drawing mode (artist), 0 = chat input */

static char input_buf[MAX_PAYLOAD];
static int input_len = 0;

static char chat_lines[MAX_CHAT_LINES][MAX_LINE_LEN];
static int chat_count = 0;

/* ncurses windows */
static WINDOW *canvas_win = NULL;
static WINDOW *chat_win = NULL;
static WINDOW *status_win = NULL;
static WINDOW *input_win = NULL;

static int chat_w = 0;  /* computed at startup */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void add_chat_line(const char *text)
{
    /* Split text on newlines and add each line separately */
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len;
        if (nl) {
            len = (int)(nl - p);
        } else {
            len = (int)strlen(p);
        }

        if (len > 0) {
            if (chat_count < MAX_CHAT_LINES) {
                snprintf(chat_lines[chat_count], MAX_LINE_LEN, "%.*s", len, p);
                chat_count++;
            } else {
                /* Shift up */
                memmove(chat_lines[0], chat_lines[1],
                        (MAX_CHAT_LINES - 1) * MAX_LINE_LEN);
                snprintf(chat_lines[MAX_CHAT_LINES - 1], MAX_LINE_LEN,
                         "%.*s", len, p);
            }
        }

        if (nl)
            p = nl + 1;
        else
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing the UI                                                      */
/* ------------------------------------------------------------------ */

static void draw_canvas(void)
{
    werase(canvas_win);
    box(canvas_win, 0, 0);

    for (int r = 0; r < CANVAS_ROWS; r++) {
        for (int c = 0; c < CANVAS_COLS; c++) {
            int pair = canvas[r][c] + 1;
            wattron(canvas_win, COLOR_PAIR(pair));
            mvwaddstr(canvas_win, r + 1, c * 2 + 1, "  ");
            wattroff(canvas_win, COLOR_PAIR(pair));
        }
    }

    /* Draw cursor for the artist */
    if (is_artist) {
        wattron(canvas_win, A_REVERSE | A_BOLD);
        mvwaddstr(canvas_win, cursor_row + 1, cursor_col * 2 + 1, "[]");
        wattroff(canvas_win, A_REVERSE | A_BOLD);
    }

    wrefresh(canvas_win);
}

static void draw_chat(void)
{
    werase(chat_win);
    box(chat_win, 0, 0);
    mvwaddstr(chat_win, 0, 2, " Chat ");

    int inner_h = CANVAS_ROWS;  /* rows available inside the box */
    int start = 0;
    if (chat_count > inner_h)
        start = chat_count - inner_h;

    for (int i = start, row = 1; i < chat_count && row <= inner_h; i++, row++) {
        mvwaddnstr(chat_win, row, 1, chat_lines[i], chat_w - 2);
    }

    wrefresh(chat_win);
}

static void draw_status(void)
{
    werase(status_win);
    wattron(status_win, A_REVERSE);

    /* Fill the bar */
    int w = CANVAS_DISPLAY_W + 2 + chat_w + 2;
    for (int i = 0; i < w; i++)
        mvwaddch(status_win, 0, i, ' ');

    if (is_artist) {
        /* Show color swatch */
        mvwprintw(status_win, 0, 1, " DRAWER ");
        wattroff(status_win, A_REVERSE);

        mvwprintw(status_win, 0, 10, "Color: ");
        wattron(status_win, COLOR_PAIR(current_color + 1));
        waddstr(status_win, "  ");
        wattroff(status_win, COLOR_PAIR(current_color + 1));
        wprintw(status_win, " %s", color_names[current_color]);

        mvwprintw(status_win, 0, 35, "| Click=draw RClick=erase C=color Z=clear Tab=chat");
    } else {
        if (has_guessed) {
            mvwprintw(status_win, 0, 1, " GUESSED! Waiting for round to end... ");
        } else {
            mvwprintw(status_win, 0, 1, " GUESSER | Type your guess and press Enter ");
        }
        wattroff(status_win, A_REVERSE);
    }

    wattroff(status_win, A_REVERSE);
    wrefresh(status_win);
}

static void draw_input(void)
{
    werase(input_win);

    if (is_artist && draw_mode) {
        mvwprintw(input_win, 0, 0, "[DRAW MODE] Press Tab to type a message");
    } else {
        mvwprintw(input_win, 0, 0, "> %.*s", input_len, input_buf);
    }

    wrefresh(input_win);
}

static void refresh_ui(void)
{
    draw_canvas();
    draw_chat();
    draw_status();
    draw_input();
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

static int send_player_join(const char *name)
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

static int send_guess(const char *text, uint32_t len)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_GUESS;
    msg.length = len;
    memcpy(msg.payload, text, len);
    return send_message(sockfd, &msg);
}

static int send_draw_cell(uint8_t row, uint8_t col, uint8_t color)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_DRAW_CELL;
    msg.payload[0] = row;
    msg.payload[1] = col;
    msg.payload[2] = color;
    msg.length = 3;
    return send_message(sockfd, &msg);
}

static int send_draw_clear(void)
{
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = MSG_DRAW_CLEAR;
    msg.length = 0;
    return send_message(sockfd, &msg);
}

/* ------------------------------------------------------------------ */
/* Handling server messages                                            */
/* ------------------------------------------------------------------ */

static int handle_server_message(const message_t *msg)
{
    switch (msg->msg_type) {
    case MSG_ROUND_START: {
        if (msg->length < 2)
            break;
        is_artist = msg->payload[0];
        has_guessed = 0;
        memset(canvas, 0, sizeof(canvas));
        cursor_row = 0;
        cursor_col = 0;

        /* Artists start in draw mode, guessers in chat mode */
        draw_mode = is_artist ? 1 : 0;

        uint32_t tlen = msg->length - 1;
        if (tlen > MAX_PAYLOAD - 1)
            tlen = MAX_PAYLOAD - 1;
        char text[MAX_PAYLOAD];
        memcpy(text, msg->payload + 1, tlen);
        text[tlen] = '\0';
        add_chat_line(text);
        break;
    }

    case MSG_CHAT: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD - 1)
            len = MAX_PAYLOAD - 1;
        char buf[MAX_PAYLOAD];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        add_chat_line(buf);
        break;
    }

    case MSG_GUESSED_NOTIFY: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD - 1)
            len = MAX_PAYLOAD - 1;
        char buf[MAX_PAYLOAD];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        has_guessed = 1;
        add_chat_line(buf);
        break;
    }

    case MSG_DRAW_CELL: {
        if (msg->length < 3)
            break;
        uint8_t row = msg->payload[0];
        uint8_t col = msg->payload[1];
        uint8_t color = msg->payload[2];
        if (row < CANVAS_ROWS && col < CANVAS_COLS && color < NUM_COLORS)
            canvas[row][col] = color;
        break;
    }

    case MSG_DRAW_CLEAR:
        memset(canvas, 0, sizeof(canvas));
        break;

    case MSG_CANVAS_SYNC:
        if (msg->length >= CANVAS_ROWS * CANVAS_COLS)
            memcpy(canvas, msg->payload, CANVAS_ROWS * CANVAS_COLS);
        break;

    case MSG_CORRECT_GUESS:
    case MSG_ROUND_END:
        is_artist = 0;
        has_guessed = 0;
        break;

    default:
        break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Key handling                                                        */
/* ------------------------------------------------------------------ */

/*
 * Returns -1 to signal quit, 0 otherwise.
 */
/*
 * Try to paint the canvas cell at screen coordinates (mouse_y, mouse_x).
 * Returns 1 if the click was on the canvas, 0 otherwise.
 */
static int try_paint_canvas(int mouse_y, int mouse_x, uint8_t color)
{
    if (!is_artist)
        return 0;

    /* Canvas window starts at (0,0); cells start at (1,1) inside the box.
     * Each cell is 2 columns wide. */
    int row = mouse_y - 1;          /* subtract box top border */
    int col = (mouse_x - 1) / 2;   /* subtract box left border, 2 cols per cell */

    if (row < 0 || row >= CANVAS_ROWS || col < 0 || col >= CANVAS_COLS)
        return 0;

    canvas[row][col] = color;
    cursor_row = row;
    cursor_col = col;
    send_draw_cell((uint8_t)row, (uint8_t)col, color);
    return 1;
}

static int handle_keypress(int ch)
{
    /* Mouse events — artist can click/drag to draw on the canvas */
    if (ch == KEY_MOUSE) {
        MEVENT event;
        if (getmouse(&event) == OK && is_artist) {
            /*
             * Button masks vary across ncurses versions.  For drag
             * (motion while held), some report REPORT_MOUSE_POSITION
             * with no button bits.  We treat ANY mouse event whose
             * coordinates fall on the canvas as a paint action, unless
             * it is a button-3 (right-click) event which erases.
             */
            int is_erase = (event.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED));
            int is_release = (event.bstate & (BUTTON1_RELEASED | BUTTON3_RELEASED));

            if (!is_release) {
                uint8_t color = is_erase ? 0 : (uint8_t)current_color;
                try_paint_canvas(event.y, event.x, color);
            }
        }
        return 0;
    }

    /* Tab toggles between draw mode and chat mode */
    if (ch == '\t') {
        if (is_artist) {
            draw_mode = !draw_mode;
        }
        return 0;
    }

    /* In draw mode (artist only) */
    if (draw_mode && is_artist) {
        switch (ch) {
        case KEY_UP:
            if (cursor_row > 0) cursor_row--;
            break;
        case KEY_DOWN:
            if (cursor_row < CANVAS_ROWS - 1) cursor_row++;
            break;
        case KEY_LEFT:
            if (cursor_col > 0) cursor_col--;
            break;
        case KEY_RIGHT:
            if (cursor_col < CANVAS_COLS - 1) cursor_col++;
            break;
        case ' ':
            canvas[cursor_row][cursor_col] = (uint8_t)current_color;
            send_draw_cell((uint8_t)cursor_row, (uint8_t)cursor_col,
                           (uint8_t)current_color);
            break;
        case 'e':
        case 'E':
            canvas[cursor_row][cursor_col] = 0;
            send_draw_cell((uint8_t)cursor_row, (uint8_t)cursor_col, 0);
            break;
        case 'c':
            current_color = (current_color % (NUM_COLORS - 1)) + 1;
            break;
        case 'C':
            current_color--;
            if (current_color < 1) current_color = NUM_COLORS - 1;
            break;
        case 'z':
        case 'Z':
            memset(canvas, 0, sizeof(canvas));
            send_draw_clear();
            break;
        default:
            break;
        }
        return 0;
    }

    /* Chat / input mode */
    switch (ch) {
    case '\n':
    case KEY_ENTER:
        if (input_len > 0) {
            send_guess(input_buf, (uint32_t)input_len);
            input_len = 0;
            input_buf[0] = '\0';
        }
        break;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (input_len > 0)
            input_buf[--input_len] = '\0';
        break;
    default:
        if (ch >= 32 && ch < 127 && input_len < (int)sizeof(input_buf) - 1) {
            input_buf[input_len++] = (char)ch;
            input_buf[input_len] = '\0';
        }
        break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* ncurses setup / teardown                                            */
/* ------------------------------------------------------------------ */

static void setup_ncurses(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);

    /* Enable mouse clicks and drag events */
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);  /* disable click-vs-doubleclick delay */
    /* Request button-event tracking (motion while button held) from terminal */
    printf("\033[?1002h");
    fflush(stdout);

    if (has_colors()) {
        start_color();
        /* Color pairs: pair N+1 = background color N
         * Pair 1 = black bg (empty), pair 2 = red bg, ... pair 8 = white bg */
        init_pair(1, COLOR_WHITE, COLOR_BLACK);
        init_pair(2, COLOR_WHITE, COLOR_RED);
        init_pair(3, COLOR_WHITE, COLOR_GREEN);
        init_pair(4, COLOR_BLACK, COLOR_YELLOW);
        init_pair(5, COLOR_WHITE, COLOR_BLUE);
        init_pair(6, COLOR_WHITE, COLOR_MAGENTA);
        init_pair(7, COLOR_BLACK, COLOR_CYAN);
        init_pair(8, COLOR_BLACK, COLOR_WHITE);
    }

    int term_w = COLS;
    chat_w = term_w - CANVAS_DISPLAY_W - 2;  /* subtract canvas box width */
    if (chat_w < CHAT_MIN_W)
        chat_w = CHAT_MIN_W;

    int canvas_win_w = CANVAS_DISPLAY_W + 2; /* +2 for box borders */
    int canvas_win_h = CANVAS_ROWS + 2;
    int chat_win_w = chat_w + 2;

    canvas_win = newwin(canvas_win_h, canvas_win_w, 0, 0);
    chat_win   = newwin(canvas_win_h, chat_win_w, 0, canvas_win_w);
    status_win = newwin(STATUS_H, term_w, canvas_win_h, 0);
    input_win  = newwin(INPUT_H, term_w, canvas_win_h + STATUS_H, 0);

    refresh();
}

static void cleanup_ncurses(void)
{
    /* Disable mouse motion tracking */
    printf("\033[?1002l");
    fflush(stdout);

    if (canvas_win) delwin(canvas_win);
    if (chat_win) delwin(chat_win);
    if (status_win) delwin(status_win);
    if (input_win) delwin(input_win);
    endwin();
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

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

    /* Resolve host and connect (before ncurses) */
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

    sockfd = sock;
    printf("Connected to %s:%d\n", host, port);

    /* Get player name (regular stdio, before ncurses) */
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

    if (send_player_join(name) < 0) {
        fprintf(stderr, "Failed to send join message\n");
        close(sockfd);
        return 1;
    }

    /* Enter ncurses mode */
    setup_ncurses();

    add_chat_line("Connected! Type \"play\" to start.");
    add_chat_line("(Press Tab to switch draw/chat mode)");
    refresh_ui();

    /* Main loop: select() on stdin + server socket */
    int running = 1;
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sockfd, &readfds);
        int maxfd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  /* 50ms for responsive UI */

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Handle keyboard input */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            int ch;
            while ((ch = getch()) != ERR) {
                if (handle_keypress(ch) < 0) {
                    running = 0;
                    break;
                }
            }
        }

        /* Handle server messages */
        if (FD_ISSET(sockfd, &readfds)) {
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            if (recv_message(sockfd, &msg) < 0) {
                add_chat_line("*** Server disconnected ***");
                refresh_ui();
                napms(2000);
                running = 0;
            } else {
                handle_server_message(&msg);
            }
        }

        if (running)
            refresh_ui();
    }

    cleanup_ncurses();
    close(sockfd);
    return 0;
}
