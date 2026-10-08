/*
 * x11-debug-app: small Xlib probe for TAWC's XWayland tests.
 *
 * Commands:
 *   paste         Read CLIPBOARD as UTF8_STRING and print TAWC_DEBUG
 *   paste-loop    Re-read CLIPBOARD every 250ms, reporting each attempt
 *   copy <text>   Own CLIPBOARD and serve text until killed
 *   window        Keep the window up until killed
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIN_W 640
#define WIN_H 240

static int running = 1;

struct atoms {
    Atom clipboard;
    Atom targets;
    Atom utf8_string;
    Atom string;
    Atom text_plain;
    Atom text_plain_utf8;
    Atom tawc_clipboard;
};

struct x11_app {
    Display *dpy;
    Window win;
    struct atoms atoms;
    const char *command;
};

struct command {
    const char *name;
    const char *usage;
    int (*run)(struct x11_app *app, int argc, char **argv);
};

static char *join_args(int argc, char **argv);
static void draw_window(struct x11_app *app);

static void fatal(const char *fmt, ...)
{
    va_list ap;

    fputs("x11-debug-app: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(1);
}

static void debug_emit(const char *tag, const char *value)
{
    if (!value || !value[0]) {
        printf("TAWC_DEBUG:%s\n", tag);
        fflush(stdout);
        return;
    }

    printf("TAWC_DEBUG:%s:", tag);
    for (const char *p = value; *p; p++) {
        switch (*p) {
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        default: fputc(*p, stdout); break;
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
}

static unsigned long named_pixel(Display *dpy, int screen, const char *name,
                                 unsigned long fallback)
{
    XColor color;
    XColor exact;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &color,
                         &exact)) {
        return color.pixel;
    }
    return fallback;
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static Atom atom(Display *dpy, const char *name)
{
    Atom value = XInternAtom(dpy, name, False);
    if (value == None)
        fatal("missing atom %s", name);
    return value;
}

static struct atoms intern_atoms(Display *dpy)
{
    return (struct atoms) {
        .clipboard = atom(dpy, "CLIPBOARD"),
        .targets = atom(dpy, "TARGETS"),
        .utf8_string = atom(dpy, "UTF8_STRING"),
        .string = XA_STRING,
        .text_plain = atom(dpy, "text/plain"),
        .text_plain_utf8 = atom(dpy, "text/plain;charset=utf-8"),
        .tawc_clipboard = atom(dpy, "_TAWC_CLIPBOARD_TEXT"),
    };
}

static int wait_x_event(Display *dpy, XEvent *ev, int timeout_ms)
{
    int fd = ConnectionNumber(dpy);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        while (XPending(dpy) > 0) {
            XNextEvent(dpy, ev);
            return 1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_nsec - start.tv_nsec) / 1000000);
        int remaining = timeout_ms - elapsed;
        if (remaining <= 0)
            return 0;

        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
        };
        int pr = poll(&pfd, 1, remaining);
        if (pr < 0 && errno != EINTR)
            fatal("poll X connection failed: %s", strerror(errno));
    }
}

static void pump_x_events(struct x11_app *app, int duration_ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_nsec - start.tv_nsec) / 1000000);
        int remaining = duration_ms - elapsed;
        if (remaining <= 0)
            return;

        XEvent ev;
        if (!wait_x_event(app->dpy, &ev, remaining))
            return;
        if (ev.type == Expose && ev.xexpose.window == app->win)
            draw_window(app);
    }
}

static void draw_window(struct x11_app *app)
{
    Display *dpy = app->dpy;
    Window win = app->win;
    int screen = DefaultScreen(dpy);
    XWindowAttributes attrs;
    int width = WIN_W;
    int height = WIN_H;
    unsigned long bg = named_pixel(dpy, screen, "#17352f",
                                   BlackPixel(dpy, screen));
    unsigned long accent = named_pixel(dpy, screen, "#f0b84d",
                                       WhitePixel(dpy, screen));
    unsigned long white = WhitePixel(dpy, screen);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (!gc)
        fatal("XCreateGC failed");
    if (XGetWindowAttributes(dpy, win, &attrs)) {
        width = attrs.width > 0 ? attrs.width : width;
        height = attrs.height > 0 ? attrs.height : height;
    }

    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, win, gc, 0, 0, width, height);
    XSetForeground(dpy, gc, accent);
    XFillRectangle(dpy, win, gc, 0, 0, 14, height);
    XDrawRectangle(dpy, win, gc, 8, 8, width - 17, height - 17);
    XSetForeground(dpy, gc, white);
    XDrawString(dpy, win, gc, 40, 58, "x11-debug-app",
                (int)strlen("x11-debug-app"));
    XDrawString(dpy, win, gc, 40, 92, app->command,
                (int)strlen(app->command));
    XFreeGC(dpy, gc);
    XFlush(dpy);
}

static void create_window(struct x11_app *app)
{
    Display *dpy = app->dpy;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 0, 0, WIN_W, WIN_H, 0,
                                     BlackPixel(dpy, screen),
                                     WhitePixel(dpy, screen));
    app->win = win;
    XStoreName(dpy, win, "x11-debug-app");
    XClassHint class_hint = {
        .res_name = "x11-debug-app",
        .res_class = "TawcX11Debug",
    };
    XSetClassHint(dpy, win, &class_hint);
    XSizeHints hints = {
        .flags = PSize,
        .width = WIN_W,
        .height = WIN_H,
    };
    XSetWMNormalHints(dpy, win, &hints);
    long pid = (long)getpid();
    XChangeProperty(dpy, win, atom(dpy, "_NET_WM_PID"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&pid, 1);
    XResizeWindow(dpy, win, WIN_W, WIN_H);
    XSelectInput(dpy, win, StructureNotifyMask | PropertyChangeMask |
                           ExposureMask);
    XMapWindow(dpy, win);
    XFlush(dpy);

    for (;;) {
        XEvent ev;
        if (!wait_x_event(dpy, &ev, 10000))
            fatal("timed out waiting for MapNotify");
        if (ev.type == Expose && ev.xexpose.window == win)
            draw_window(app);
        if (ev.type == MapNotify && ev.xmap.window == win)
            break;
    }

    draw_window(app);
    debug_emit("READY", NULL);
}

static int is_text_target(const struct atoms *atoms, Atom target)
{
    return target == atoms->utf8_string ||
           target == atoms->string ||
           target == atoms->text_plain ||
           target == atoms->text_plain_utf8;
}

/* One CLIPBOARD -> UTF8_STRING conversion. Returns the text (caller
 * frees) or NULL when the selection owner refused the request, which is
 * how the compositor answers a client that doesn't hold focus. */
static char *convert_clipboard(struct x11_app *app)
{
    Display *dpy = app->dpy;
    Window win = app->win;
    const struct atoms *atoms = &app->atoms;
    XEvent ev;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    XConvertSelection(dpy, atoms->clipboard, atoms->utf8_string,
                      atoms->tawc_clipboard, win, CurrentTime);
    XFlush(dpy);

    for (;;) {
        if (!wait_x_event(dpy, &ev, 10000))
            fatal("timed out waiting for SelectionNotify");
        if (ev.type == Expose && ev.xexpose.window == win)
            draw_window(app);
        else if (ev.type == SelectionNotify)
            break;
    }

    if (ev.xselection.property == None)
        return NULL;

    if (XGetWindowProperty(dpy, win, atoms->tawc_clipboard, 0, 1024 * 1024,
                           True, AnyPropertyType, &actual_type,
                           &actual_format, &nitems, &bytes_after,
                           &data) != Success) {
        fatal("XGetWindowProperty failed");
    }
    if (actual_format != 8 || !data)
        fatal("clipboard property had unexpected format %d", actual_format);
    if (bytes_after != 0)
        fatal("clipboard property exceeded test buffer");

    char *text = calloc(nitems + 1, 1);
    if (!text)
        fatal("calloc failed");
    memcpy(text, data, nitems);
    XFree(data);
    return text;
}

static int cmd_paste(struct x11_app *app, int argc, char **argv)
{
    (void)argv;
    if (argc != 0)
        fatal("paste takes no arguments");

    /* Let the Rust test inject a tap after READY so XWayland grants
     * selection access to this focused X11 client. Keep painting while
     * the compositor resizes the X11 window to the host. */
    pump_x_events(app, 2000);

    char *text = convert_clipboard(app);
    if (!text)
        fatal("selection owner refused UTF8_STRING");
    debug_emit("CLIPBOARD_PASTE", text);
    free(text);
    return 0;
}

/* Poll CLIPBOARD until killed, reporting every attempt as
 * `CLIPBOARD_TRY:ok=<text>` or `CLIPBOARD_TRY:denied`. Lets a test watch
 * one client's access change as focus moves to and from another. */
static int cmd_paste_loop(struct x11_app *app, int argc, char **argv)
{
    (void)argv;
    if (argc != 0)
        fatal("paste-loop takes no arguments");

    while (running) {
        char *text = convert_clipboard(app);
        if (text) {
            char *line = calloc(strlen(text) + sizeof("ok="), 1);
            if (!line)
                fatal("calloc failed");
            strcpy(line, "ok=");
            strcat(line, text);
            debug_emit("CLIPBOARD_TRY", line);
            free(line);
            free(text);
        } else {
            debug_emit("CLIPBOARD_TRY", "denied");
        }
        pump_x_events(app, 250);
    }
    return 0;
}

static void handle_selection_request(Display *dpy, const struct atoms *atoms,
                                     const XSelectionRequestEvent *req,
                                     const char *text)
{
    XSelectionEvent reply = {
        .type = SelectionNotify,
        .display = req->display,
        .requestor = req->requestor,
        .selection = req->selection,
        .target = req->target,
        .property = None,
        .time = req->time,
    };
    Atom property = req->property == None ? req->target : req->property;

    if (req->target == atoms->targets) {
        Atom supported[] = {
            atoms->targets,
            atoms->utf8_string,
            atoms->string,
            atoms->text_plain,
            atoms->text_plain_utf8,
        };
        XChangeProperty(dpy, req->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)supported,
                        (int)(sizeof(supported) / sizeof(supported[0])));
        reply.property = property;
    } else if (is_text_target(atoms, req->target)) {
        XChangeProperty(dpy, req->requestor, property, req->target, 8,
                        PropModeReplace, (const unsigned char *)text,
                        (int)strlen(text));
        reply.property = property;
        char *name = XGetAtomName(dpy, req->target);
        debug_emit("CLIPBOARD_SEND", name ? name : "");
        if (name)
            XFree(name);
    }

    XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
    XFlush(dpy);
}

static int cmd_copy(struct x11_app *app, int argc, char **argv)
{
    if (argc < 1)
        fatal("copy requires text");
    Display *dpy = app->dpy;
    Window win = app->win;
    const struct atoms *atoms = &app->atoms;
    char *text = join_args(argc, argv);

    XSetSelectionOwner(dpy, atoms->clipboard, win, CurrentTime);
    XFlush(dpy);
    if (XGetSelectionOwner(dpy, atoms->clipboard) != win)
        fatal("failed to own CLIPBOARD");

    debug_emit("CLIPBOARD_SET", text);
    while (running) {
        XEvent ev;
        if (!wait_x_event(dpy, &ev, 250))
            continue;
        if (ev.type == Expose && ev.xexpose.window == win)
            draw_window(app);
        else if (ev.type == SelectionRequest)
            handle_selection_request(dpy, atoms, &ev.xselectionrequest, text);
        else if (ev.type == SelectionClear)
            running = 0;
    }
    free(text);
    return 0;
}

static int cmd_window(struct x11_app *app, int argc, char **argv)
{
    (void)argv;
    if (argc != 0)
        fatal("window takes no arguments");

    while (running)
        pump_x_events(app, 250);
    return 0;
}

static char *join_args(int argc, char **argv)
{
    size_t len = 0;
    for (int i = 0; i < argc; i++)
        len += strlen(argv[i]) + 1;

    char *text = calloc(len + 1, 1);
    if (!text)
        fatal("calloc failed");
    for (int i = 0; i < argc; i++) {
        if (i > 0)
            strcat(text, " ");
        strcat(text, argv[i]);
    }
    return text;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        fatal("usage: %s paste|paste-loop|copy <text>|window", argv[0]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const struct command commands[] = {
        { "paste", "paste", cmd_paste },
        { "paste-loop", "paste-loop", cmd_paste_loop },
        { "copy", "copy <text>", cmd_copy },
        { "window", "window", cmd_window },
    };
    const struct command *cmd = NULL;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(argv[1], commands[i].name) == 0) {
            cmd = &commands[i];
            break;
        }
    }
    if (!cmd)
        fatal("unknown command: %s", argv[1]);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        fatal("XOpenDisplay failed; DISPLAY=%s", getenv("DISPLAY"));
    struct x11_app app = {
        .dpy = dpy,
        .win = 0,
        .atoms = intern_atoms(dpy),
        .command = cmd->usage,
    };
    create_window(&app);
    int rc = cmd->run(&app, argc - 2, argv + 2);
    XCloseDisplay(dpy);
    return rc;
}
