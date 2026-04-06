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
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "net.h"

/* Layout */
#define CELL_SIZE    16          /* pixels per canvas cell */
#define CANVAS_PX_W  (CANVAS_COLS * CELL_SIZE)  /* 640 */
#define CANVAS_PX_H  (CANVAS_ROWS * CELL_SIZE)  /* 320 */
#define CHAT_W       300        /* chat width */
#define TOOLBAR_H    40         /* color palette bar height */
#define INPUT_H      28         /* text input bar height */
#define WIN_W        (CANVAS_PX_W + CHAT_W)
#define WIN_H        (CANVAS_PX_H + TOOLBAR_H + INPUT_H)
#define CHAT_LINE_H  16         /* pixels per chat line */
#define MAX_CHAT_LINES 200
#define MAX_LINE_LEN   256

/* X11 */
static Display *dpy = NULL;
static Window   win;
static GC       gc;
static Atom     wm_delete;
static XFontStruct *font = NULL;

/* Colours */
static unsigned long xcolors[NUM_COLORS];
static const char *color_hex[NUM_COLORS] = {
    "#000000", "#FF0000", "#00CC00", "#DDDD00",
    "#0000FF", "#CC00CC", "#00CCCC", "#FFFFFF"
};

/* Network */
static int sockfd = -1;

/* Game state */
static int is_artist = 0;
static int has_guessed = 0;
static uint8_t canvas[CANVAS_ROWS][CANVAS_COLS];
static int current_color = 1;
static int mouse_held = 0;

/* Chat */
static char chat_lines[MAX_CHAT_LINES][MAX_LINE_LEN];
static int chat_count = 0;

/* Text input */
static char input_buf[MAX_PAYLOAD];
static int input_len = 0;

/* Chat functions */

static void add_chat_line(const char *text)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        if (len > 0) {
            if (chat_count < MAX_CHAT_LINES) {
                snprintf(chat_lines[chat_count], MAX_LINE_LEN, "%.*s", len, p);
                chat_count++;
            } else {
                memmove(chat_lines[0], chat_lines[1],
                        (MAX_CHAT_LINES - 1) * MAX_LINE_LEN);
                snprintf(chat_lines[MAX_CHAT_LINES - 1], MAX_LINE_LEN,
                        "%.*s", len, p);
            }
        }
        p = nl ? nl + 1 : p + strlen(p);
    }
}

/* Network functions */

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

/* Server messages*/

static void handle_server_message(const message_t *msg)
{
    switch (msg->msg_type) {
    case MSG_ROUND_START: {
        if (msg->length < 2) break;
        is_artist = msg->payload[0];
        has_guessed = 0;
        memset(canvas, 0, sizeof(canvas));

        uint32_t tlen = msg->length - 1;
        if (tlen > MAX_PAYLOAD - 1) tlen = MAX_PAYLOAD - 1;
        char text[MAX_PAYLOAD];
        memcpy(text, msg->payload + 1, tlen);
        text[tlen] = '\0';
        add_chat_line(text);
        break;
    }
    case MSG_CHAT: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD - 1) len = MAX_PAYLOAD - 1;
        char buf[MAX_PAYLOAD];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        add_chat_line(buf);
        break;
    }
    case MSG_GUESSED_NOTIFY: {
        uint32_t len = msg->length;
        if (len > MAX_PAYLOAD - 1) len = MAX_PAYLOAD - 1;
        char buf[MAX_PAYLOAD];
        memcpy(buf, msg->payload, len);
        buf[len] = '\0';
        has_guessed = 1;
        add_chat_line(buf);
        break;
    }
    case MSG_DRAW_CELL: {
        if (msg->length < 3) break;
        uint8_t r = msg->payload[0];
        uint8_t c = msg->payload[1];
        uint8_t col = msg->payload[2];
        if (r < CANVAS_ROWS && c < CANVAS_COLS && col < NUM_COLORS)
            canvas[r][c] = col;
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
}

/* UI */

static void paint_cell(int r, int c)
{
    XSetForeground(dpy, gc, xcolors[(int)canvas[r][c]]);
    XFillRectangle(dpy, win, gc, c * CELL_SIZE, r * CELL_SIZE,
                   CELL_SIZE, CELL_SIZE);

    XSetForeground(dpy, gc, xcolors[0]);
    XDrawRectangle(dpy, win, gc, c * CELL_SIZE, r * CELL_SIZE,
                   CELL_SIZE - 1, CELL_SIZE - 1);
}

static void redraw_all(void)
{
    /* Background */
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
    XFillRectangle(dpy, win, gc, 0, 0, WIN_W, WIN_H);

    /* Canvas */
    for (int r = 0; r < CANVAS_ROWS; r++)
        for (int c = 0; c < CANVAS_COLS; c++)
            paint_cell(r, c);

    /* Canvas border */
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawRectangle(dpy, win, gc, 0, 0, CANVAS_PX_W - 1, CANVAS_PX_H - 1);

    /* Toolbar */
    int ty = CANVAS_PX_H;

    /* Divider line */
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawLine(dpy, win, gc, 0, ty, WIN_W, ty);

    /* Color palette swatches */
    int swatch_size = 24;
    int pad = 6;
    for (int i = 0; i < NUM_COLORS; i++) {
        int sx = pad + i * (swatch_size + pad);
        int sy = ty + (TOOLBAR_H - swatch_size) / 2;
        XSetForeground(dpy, gc, xcolors[i]);
        XFillRectangle(dpy, win, gc, sx, sy, swatch_size, swatch_size);
        /* Highlight selected color */
        if (i == current_color) {
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XSetLineAttributes(dpy, gc, 3, LineSolid, CapButt, JoinMiter);
            XDrawRectangle(dpy, win, gc, sx - 2, sy - 2,
                            swatch_size + 3, swatch_size + 3);
            XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
        } else {
            XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
            XDrawRectangle(dpy, win, gc, sx, sy,
                            swatch_size - 1, swatch_size - 1);
        }
    }

    /* Clear button */
    int clr_x = pad + NUM_COLORS * (swatch_size + pad) + 20;
    int clr_y = ty + (TOOLBAR_H - swatch_size) / 2;
    XSetForeground(dpy, gc, 0xDDDDDD);
    XFillRectangle(dpy, win, gc, clr_x, clr_y, 60, swatch_size);
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawRectangle(dpy, win, gc, clr_x, clr_y, 59, swatch_size - 1);
    if (font) {
        XDrawString(dpy, win, gc, clr_x + 10, clr_y + 17, "Clear", 5);
    }

    /* Role label */
    int label_x = clr_x + 80;
    if (is_artist) {
        XSetForeground(dpy, gc, 0x0000CC);
        if (font) XDrawString(dpy, win, gc, label_x, ty + 25,
                                "DRAWER", 6);
    } else {
        XSetForeground(dpy, gc, 0x008800);
        const char *lbl = has_guessed ? "GUESSED!" : "GUESSER";
        if (font) XDrawString(dpy, win, gc, label_x, ty + 25,
                                lbl, (int)strlen(lbl));
    }

    /* Chat panel */
    int cx = CANVAS_PX_W;

    /* Vertical divider */
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawLine(dpy, win, gc, cx, 0, cx, CANVAS_PX_H);

    /* Chat header */
    XSetForeground(dpy, gc, 0xEEEEEE);
    XFillRectangle(dpy, win, gc, cx + 1, 0, CHAT_W - 1, 20);
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawLine(dpy, win, gc, cx, 20, cx + CHAT_W, 20);
    if (font) XDrawString(dpy, win, gc, cx + 8, 15, "Chat", 4);

    /* Chat lines */
    int visible = (CANVAS_PX_H - 22) / CHAT_LINE_H;
    int start = 0;
    if (chat_count > visible)
        start = chat_count - visible;

    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    for (int i = start, row = 0; i < chat_count && row < visible; i++, row++) {
        int ty2 = 22 + CHAT_LINE_H + row * CHAT_LINE_H;
        int char_w = font ? font->max_bounds.width : 6;
        int maxchars = (CHAT_W - 12) / char_w;
        int len = (int)strlen(chat_lines[i]);
        if (len > maxchars) len = maxchars;
        if (font) XDrawString(dpy, win, gc, cx + 6, ty2,
                                chat_lines[i], len);
    }

    /* Input bar */
    int iy = CANVAS_PX_H + TOOLBAR_H;
    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    XDrawLine(dpy, win, gc, 0, iy, WIN_W, iy);

    XSetForeground(dpy, gc, 0xF5F5F5);
    XFillRectangle(dpy, win, gc, 0, iy + 1, WIN_W, INPUT_H - 1);

    XSetForeground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));
    if (font) {
        char prompt[MAX_PAYLOAD + 4];
        snprintf(prompt, sizeof(prompt), "> %.*s_", input_len, input_buf);
        XDrawString(dpy, win, gc, 8, iy + 19, prompt, (int)strlen(prompt));
    }

    XFlush(dpy);
}

/* Canvas mouse interaction */

static void try_paint(int px, int py, uint8_t color)
{
    if (!is_artist) return;
    if (px < 0 || px >= CANVAS_PX_W || py < 0 || py >= CANVAS_PX_H)
        return;

    int col = px / CELL_SIZE;
    int row = py / CELL_SIZE;
    if (row >= CANVAS_ROWS || col >= CANVAS_COLS) return;

    if (canvas[row][col] == color) return;

    canvas[row][col] = color;
    send_draw_cell((uint8_t)row, (uint8_t)col, color);

    paint_cell(row, col);
    XFlush(dpy);
}

static int toolbar_hit_color(int px, int py)
{
    int ty = CANVAS_PX_H;
    int swatch_size = 24;
    int pad = 6;

    for (int i = 0; i < NUM_COLORS; i++) {
        int sx = pad + i * (swatch_size + pad);
        int sy = ty + (TOOLBAR_H - swatch_size) / 2;
        if (px >= sx && px < sx + swatch_size &&
            py >= sy && py < sy + swatch_size)
            return i;
    }
    return -1;
}

static int toolbar_hit_clear(int px, int py)
{
    int ty = CANVAS_PX_H;
    int swatch_size = 24;
    int pad = 6;
    int clr_x = pad + NUM_COLORS * (swatch_size + pad) + 20;
    int clr_y = ty + (TOOLBAR_H - swatch_size) / 2;
    return (px >= clr_x && px < clr_x + 60 &&
            py >= clr_y && py < clr_y + swatch_size);
}


 * Returns -1 to quit, 0 otherwise.
 */
static int handle_x11_events(void)
{
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                redraw_all();
            break;

        case ButtonPress: {
            int px = ev.xbutton.x;
            int py = ev.xbutton.y;

            if (ev.xbutton.button == 1) {
                int ci = toolbar_hit_color(px, py);
                if (ci >= 0 && is_artist) {
                    current_color = ci;
                    redraw_all();
                    break;
                }
                if (toolbar_hit_clear(px, py) && is_artist) {
                    memset(canvas, 0, sizeof(canvas));
                    send_draw_clear();
                    redraw_all();
                    break;
                }

                mouse_held = 1;
                try_paint(px, py, (uint8_t)current_color);
            } else if (ev.xbutton.button == 3) {
                try_paint(px, py, 0);
            }
            break;
        }

        case ButtonRelease:
            if (ev.xbutton.button == 1)
                mouse_held = 0;
            break;

        case MotionNotify:
            if (mouse_held && is_artist) {
                while (XCheckMaskEvent(dpy, PointerMotionMask, &ev))
                    ;
                try_paint(ev.xmotion.x, ev.xmotion.y,
                            (uint8_t)current_color);
            }
            break;

        case KeyPress: {
            char buf[32];
            KeySym ksym;
            int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1,
                                    &ksym, NULL);

            if (ksym == XK_Return || ksym == XK_KP_Enter) {
                if (input_len > 0) {
                    send_guess(input_buf, (uint32_t)input_len);
                    input_len = 0;
                    input_buf[0] = '\0';
                    redraw_all();
                }
            } else if (ksym == XK_BackSpace) {
                if (input_len > 0) {
                    input_buf[--input_len] = '\0';
                    redraw_all();
                }
            } else if (ksym == XK_Escape) {
                return -1;
            } else if (n > 0 && buf[0] >= 32 && buf[0] < 127) {
                if (input_len < (int)sizeof(input_buf) - 1) {
                    input_buf[input_len++] = buf[0];
                    input_buf[input_len] = '\0';
                    redraw_all();
                }
            }
            break;
        }

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete)
                return -1;
            break;

        default:
            break;
        }
    }
    return 0;
}

/* X11 setup */

static int setup_x11(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display. Is DISPLAY set?\n");
        return -1;
    }

    int screen = DefaultScreen(dpy);
    unsigned long black = BlackPixel(dpy, screen);
    unsigned long white = WhitePixel(dpy, screen);

    int pid_offset = (int)(getpid() % 6) * 30;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                            50 + pid_offset, 50 + pid_offset, WIN_W, WIN_H,
                            1, black, white);

    XStoreName(dpy, win, "Scribble");

    XSelectInput(dpy, win,
                    ExposureMask | KeyPressMask |
                    ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask);

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = WIN_W;
        hints->min_height = hints->max_height = WIN_H;
        XSetWMNormalHints(dpy, win, hints);
        XFree(hints);
    }

    gc = XCreateGC(dpy, win, 0, NULL);

    /* Load a basic fixed-width font */
    font = XLoadQueryFont(dpy, "fixed");
    if (!font)
        font = XLoadQueryFont(dpy, "*-fixed-medium-r-*-*-13-*");
    if (font)
        XSetFont(dpy, gc, font->fid);

    /* Allocate colors */
    Colormap cmap = DefaultColormap(dpy, screen);
    for (int i = 0; i < NUM_COLORS; i++) {
        XColor xc;
        XParseColor(dpy, cmap, color_hex[i], &xc);
        XAllocColor(dpy, cmap, &xc);
        xcolors[i] = xc.pixel;
    }

    XMapWindow(dpy, win);
    XFlush(dpy);

    return 0;
}

static void cleanup_x11(void)
{
    if (font) XFreeFont(dpy, font);
    if (gc) XFreeGC(dpy, gc);
    if (dpy) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }
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

    signal(SIGPIPE, SIG_IGN);

    /* Resolve and connect */
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
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
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

    /* Get player name (stdio, before X11) */
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

    /* Open X11 window */
    if (setup_x11() < 0) {
        close(sockfd);
        return 1;
    }

    add_chat_line("Connected! Type \"play\" to start.");
    memset(canvas, 0, sizeof(canvas));

    /* Main loop: select() on X11 fd + server socket */
    int x11_fd = ConnectionNumber(dpy);
    int running = 1;

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(x11_fd, &readfds);
        FD_SET(sockfd, &readfds);
        int maxfd = x11_fd > sockfd ? x11_fd : sockfd;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  /* 50ms */

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* X11 events */
        if (FD_ISSET(x11_fd, &readfds) || XPending(dpy)) {
            if (handle_x11_events() < 0) {
                running = 0;
                break;
            }
        }

        /* Server messages */
        if (FD_ISSET(sockfd, &readfds)) {
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            if (recv_message(sockfd, &msg) < 0) {
                add_chat_line("*** Server disconnected ***");
                redraw_all();
                sleep(2);
                running = 0;
            } else {
                handle_server_message(&msg);
                redraw_all();
            }
        }
    }

    cleanup_x11();
    close(sockfd);
    return 0;
}
