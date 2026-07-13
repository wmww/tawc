/*
 * wayland-debug-app: toolkitless Wayland client for tawc integration tests.
 *
 * Subcommand CLI; output is the TAWC_DEBUG: protocol parsed by the Rust
 * integration harness.
 *
 * This is test code, not production UI. Missing globals, failed Wayland
 * requests, invalid protocol ordering, compositor protocol violations,
 * truncated protocol strings, and internal state invariants abort the process
 * so the harness sees a loud failure instead of a tolerant client hiding a
 * compositor bug. Do not make this app tolerate protocol violations; add an
 * explicit assertion for the rule being tested.
 *
 * Usage: wayland-debug-app <command>
 *
 * Commands:
 *   text-input                  Minimal editable text-input-v3 surface
 *   text-input-no-surrounding   Text-input surface that never sends surrounding
 *   text-input-stale-newline    Reports cursor before trailing newlines
 *   text-input-echo-preedit     Includes active preedit in surrounding text
 *                               (Qt/KTextEditor style) and reports after
 *                               preedit-only changes
 *   clipboard-copy <text>       Set wl_data_device clipboard text
 *   clipboard-copy-double <text> Set clipboard text twice per copy (GTK3 style)
 *   clipboard-copy-overcap      Set a clipboard source larger than 1 MiB
 *   clipboard-copy-timeout      Set a clipboard source that never closes
 *   clipboard-paste             Read focused wl_data_device clipboard text
 *   touch                       Fullscreen touch visualizer
 *   subsurface                  Fullscreen toplevel with a touchable subsurface
 *   popup                       Fullscreen toplevel with a touchable xdg_popup
 *   popup-switch                Two touch-opened grabbed xdg_popups
 *   render-pattern              Fullscreen deterministic SHM color pattern
 *   scale                       Report wp_fractional_scale_v1 changes
 *   initial-configure           Report initial output/configure sequencing
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <wayland-client.h>

#include "text-input-unstable-v3-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define WIN_W 640
#define WIN_H 240
#define CHILD_W 180
#define CHILD_H 140
#define POPUP_SHADOW 12
#define POPUP_PARENT_GEOM_X 17
#define POPUP_PARENT_GEOM_Y 9
#define POPUP_PARENT_GEOM_R 11
#define POPUP_PARENT_GEOM_B 7
#define TAP_FRAC_X 0.30
#define TAP_FRAC_Y 0.35
#define TEXT_X 24.0
#define TEXT_Y 96.0
#define FONT_SIZE 32.0
#define APPROX_CHAR_W 19.0
#define MAX_TEXT 8192
#define CLIPBOARD_OVERCAP_BYTES (1024 * 1024 + 4096)
#define MAX_TOUCHES 16
#define MAIN_BUFFER_COUNT 2

#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_ENTER 28
#define KEY_DELETE 111

/* --- Fail-fast helpers -------------------------------------------------- */

static void fatal(const char *fmt, ...)
{
    va_list ap;

    fputs("wayland-debug-app: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

static void require_true(int condition, const char *fmt, ...)
{
    va_list ap;

    if (condition)
        return;

    fputs("wayland-debug-app: assertion failed: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

static void checked_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size)
        fatal("string formatting overflow");
}

static void checked_copy(char *dst, size_t size, const char *src,
                         const char *label)
{
    size_t n = strlen(src);
    if (n >= size)
        fatal("%s too long: %zu bytes, max %zu", label, n, size - 1);
    memcpy(dst, src, n + 1);
}

static void checked_flush(struct wl_display *display)
{
    if (wl_display_flush(display) < 0)
        fatal("wl_display_flush failed: %s", strerror(errno));
}

/* --- Output protocol ---------------------------------------------------- */

static void debug_emit(const char *tag, const char *value)
{
    if (!value || !value[0]) {
        printf("TAWC_DEBUG:%s\n", tag);
        fflush(stdout);
        return;
    }

    fputs("TAWC_DEBUG:", stdout);
    fputs(tag, stdout);
    fputc(':', stdout);
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

static void debug_emit_u32(const char *tag, uint32_t value)
{
    char buf[32];
    checked_snprintf(buf, sizeof(buf), "%u", value);
    debug_emit(tag, buf);
}

static void debug_emit_i32_pair(const char *tag, int32_t a, int32_t b)
{
    char buf[64];
    checked_snprintf(buf, sizeof(buf), "%d %d", a, b);
    debug_emit(tag, buf);
}

/* --- App state ---------------------------------------------------------- */

struct app;
struct main_shm_pool;

struct shm_buffer {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    struct main_shm_pool *pool;
    int busy;
    int index;
};

struct main_shm_pool {
    struct app *app;
    void *data;
    size_t size;
    int width;
    int height;
    int stride;
    int retired;
    int live_buffers;
    struct shm_buffer buffers[MAIN_BUFFER_COUNT];
};

/* text-input-v3 delivers edits as a transaction terminated by `done`.
 * Keep callbacks side-effect free until then so tests catch compositor
 * ordering bugs instead of accidentally passing because this client was
 * more forgiving than the protocol. */
struct text_input_pending {
    int has_preedit;
    int has_commit;
    int has_delete;
    char preedit[MAX_TEXT];
    char commit[MAX_TEXT];
    uint32_t before_length;
    uint32_t after_length;
};

struct touch_point {
    int active;
    int seen;
    int32_t id;
    double x;
    double y;
    char target[16];
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct wl_data_offer *clipboard_offer;
    struct wl_data_source *clipboard_source;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_output *output;
    struct wl_surface *child_surface;
    struct wl_subsurface *child_subsurface;
    struct wl_callback *child_ready_callback;
    struct xdg_surface *child_xdg_surface;
    struct xdg_popup *child_popup;
    struct wl_surface *second_child_surface;
    struct wl_callback *second_child_ready_callback;
    struct xdg_surface *second_child_xdg_surface;
    struct xdg_popup *second_child_popup;
    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_v3 *text_input;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zxdg_toplevel_decoration_v1 *decoration;

    int running;
    int configured;
    int ready_emitted;
    int text_input_enabled;
    uint32_t text_input_commit_serial;
    int editable;
    int provide_surrounding;
    uint32_t content_hint;
    uint32_t content_purpose;
    int stale_trailing_newline_cursor;
    int echo_preedit_in_surrounding;
    int touch_debug;
    int fullscreen;
    int report_scale;
    int render_pattern;
    int request_decoration;
    int use_data_device;
    int copy_clipboard_on_focus;
    int paste_clipboard_on_selection;
    int clipboard_set;
    int clipboard_double_set;
    size_t clipboard_generated_bytes;
    int clipboard_leave_fd_open;
    int scene_kind;
    int scene_child_input_empty;
    int scene_child_created;
    int scene_child_ready;
    int child_x;
    int child_y;
    int popup_configure_x;
    int popup_configure_y;
    int popup_configure_w;
    int popup_configure_h;
    int popup_switch_created;
    int output_global_count;
    uint32_t last_preferred_scale;
    int win_w;
    int win_h;
    int main_next_buffer;
    int redraw_pending;
    struct main_shm_pool *main_pool;
    wl_fixed_t pointer_x;
    uint32_t keyboard_enter_serial;

    char text[MAX_TEXT];
    char clipboard_copy_text[MAX_TEXT];
    char clipboard_offer_mime[64];
    size_t text_len;
    size_t cursor;
    char preedit[MAX_TEXT];
    struct text_input_pending pending_text_input;
    struct touch_point touches[MAX_TOUCHES];
    int held_clipboard_fd;
};

enum scene_kind {
    SCENE_NONE = 0,
    SCENE_SUBSURFACE = 1,
    SCENE_POPUP = 2,
    SCENE_POPUP_SWITCH = 3,
};

struct wayland_mode {
    const char *title;
    const char *app_id;
    int use_text_input;
    int editable;
    int provide_surrounding;
    uint32_t content_hint;
    uint32_t content_purpose;
    int stale_trailing_newline_cursor;
    int echo_preedit_in_surrounding;
    int touch_debug;
    int fullscreen;
    int report_scale;
    int render_pattern;
    int request_decoration;
    int use_data_device;
    const char *clipboard_copy_text;
    int clipboard_double_set;
    size_t clipboard_generated_bytes;
    int clipboard_leave_fd_open;
    int clipboard_paste;
    int scene_kind;
    int scene_child_input_empty;
};

static struct app *signal_app;

/* --- Text editing ------------------------------------------------------- */

static size_t prev_char_start(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    size_t i = pos - 1;
    while (i > 0 && (((unsigned char)s[i] & 0xc0) == 0x80))
        i--;
    return i;
}

static size_t next_char_end(const char *s, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    size_t i = pos + 1;
    while (i < len && (((unsigned char)s[i] & 0xc0) == 0x80))
        i++;
    return i;
}

static size_t clamp_utf8_boundary(const char *s, size_t len, size_t pos)
{
    if (pos > len)
        pos = len;
    while (pos > 0 && pos < len && (((unsigned char)s[pos] & 0xc0) == 0x80))
        pos--;
    return pos;
}

static int is_utf8_boundary(const char *s, size_t len, size_t pos)
{
    if (pos > len)
        return 0;
    return pos == len || (((unsigned char)s[pos] & 0xc0) != 0x80);
}

static uint32_t char_count_to_byte(const char *s, size_t len, uint32_t chars)
{
    size_t pos = 0;
    while (pos < len && chars > 0) {
        pos = next_char_end(s, len, pos);
        chars--;
    }
    return (uint32_t)pos;
}

static uint32_t byte_to_char_count(const char *s, size_t len, size_t pos)
{
    uint32_t chars = 0;
    size_t i = 0;
    pos = clamp_utf8_boundary(s, len, pos);
    while (i < pos) {
        i = next_char_end(s, len, i);
        chars++;
    }
    return chars;
}

static void delete_range(struct app *app, size_t start, size_t end)
{
    require_true(is_utf8_boundary(app->text, app->text_len, start),
                 "delete start splits UTF-8 sequence: %zu", start);
    require_true(is_utf8_boundary(app->text, app->text_len, end),
                 "delete end splits UTF-8 sequence: %zu", end);
    if (start >= end)
        return;
    memmove(app->text + start, app->text + end, app->text_len - end + 1);
    app->text_len -= end - start;
    app->cursor = start;
}

static void insert_text(struct app *app, const char *text)
{
    size_t n = strlen(text);
    if (n == 0)
        return;
    if (app->text_len + n >= MAX_TEXT)
        fatal("text buffer overflow inserting %zu bytes at len %zu", n,
              app->text_len);
    memmove(app->text + app->cursor + n, app->text + app->cursor,
            app->text_len - app->cursor + 1);
    memcpy(app->text + app->cursor, text, n);
    app->cursor += n;
    app->text_len += n;
}

static void emit_text_and_cursor(struct app *app)
{
    debug_emit("TEXT_CHANGED", app->text);
    debug_emit_u32("CURSOR_POS",
                   byte_to_char_count(app->text, app->text_len, app->cursor));
}

/* --- Wayland/text-input output ----------------------------------------- */

static void sync_surrounding(struct app *app, uint32_t cause)
{
    if (!app->provide_surrounding || !app->text_input ||
        !app->text_input_enabled)
        return;
    const char *text = app->text;
    int32_t cursor = (int32_t)app->cursor;
    if (app->stale_trailing_newline_cursor &&
        cause == ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD) {
        size_t stale_cursor = app->text_len;
        while (stale_cursor > 0 && app->text[stale_cursor - 1] == '\n')
            stale_cursor--;
        if (stale_cursor < app->text_len)
            cursor = (int32_t)stale_cursor;
    }

    /* Qt/KTextEditor render preedit by inserting it into the document
     * buffer, so their surrounding-text reports include the active
     * preedit with the cursor after it — violating the protocol's
     * "excluding the preedit text" rule. Model that so compositor-side
     * echo tolerance has coverage (the Kate cumulative-commit bug). */
    static char echo_buf[2 * MAX_TEXT];
    if (app->echo_preedit_in_surrounding && app->preedit[0]) {
        size_t pre_len = strlen(app->preedit);
        memcpy(echo_buf, app->text, app->cursor);
        memcpy(echo_buf + app->cursor, app->preedit, pre_len);
        memcpy(echo_buf + app->cursor + pre_len, app->text + app->cursor,
               app->text_len - app->cursor + 1);
        text = echo_buf;
        cursor = (int32_t)(app->cursor + pre_len);
    }

    zwp_text_input_v3_set_text_change_cause(app->text_input, cause);
    zwp_text_input_v3_set_surrounding_text(app->text_input, text, cursor,
                                           cursor);
    zwp_text_input_v3_commit(app->text_input);
    app->text_input_commit_serial++;
    checked_flush(app->display);
}

static void request_redraw(struct app *app);

static void changed_by_input_method(struct app *app)
{
    emit_text_and_cursor(app);
    sync_surrounding(app, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
    request_redraw(app);
}

static void cursor_changed_by_user(struct app *app)
{
    debug_emit_u32("CURSOR_POS",
                   byte_to_char_count(app->text, app->text_len, app->cursor));
    sync_surrounding(app, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
    request_redraw(app);
}

static void apply_text_input_transaction(struct app *app)
{
    struct text_input_pending *pending = &app->pending_text_input;
    int changed = 0;
    int preedit_changed = 0;

    if (pending->has_delete) {
        size_t start = app->cursor > pending->before_length
                           ? app->cursor - pending->before_length
                           : 0;
        size_t end = app->cursor + pending->after_length;
        if (end > app->text_len)
            end = app->text_len;
        delete_range(app, start, end);
        changed = 1;
    }

    if (pending->has_commit) {
        if (app->preedit[0]) {
            app->preedit[0] = '\0';
            debug_emit("PREEDIT", "");
        }
        insert_text(app, pending->commit);
        changed = 1;
    }

    if (pending->has_preedit) {
        if (strcmp(app->preedit, pending->preedit) != 0) {
            checked_copy(app->preedit, sizeof(app->preedit),
                         pending->preedit, "preedit");
            debug_emit("PREEDIT", app->preedit);
            preedit_changed = 1;
        }
    }

    memset(pending, 0, sizeof(*pending));

    if (changed) {
        changed_by_input_method(app);
    } else if (preedit_changed) {
        /* Preedit-only change: Qt/KTextEditor still push a surrounding
         * report because for them the preedit lives in the buffer. */
        if (app->echo_preedit_in_surrounding)
            sync_surrounding(app,
                             ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
        request_redraw(app);
    }
}

static void apply_surroundingless_text_input_transaction(struct app *app)
{
    struct text_input_pending *pending = &app->pending_text_input;

    if (pending->has_preedit)
        debug_emit("PREEDIT", pending->preedit);

    if (pending->has_commit)
        debug_emit("COMMIT", pending->commit);

    if (pending->has_delete) {
        char buf[64];
        checked_snprintf(buf, sizeof(buf), "%u:%u", pending->before_length,
                         pending->after_length);
        debug_emit("DELETE_SURROUNDING", buf);
    }

    memset(pending, 0, sizeof(*pending));
}

/* --- Clipboard / wl_data_device --------------------------------------- */

static int clipboard_mime_rank(const char *mime)
{
    if (strcmp(mime, "text/plain;charset=utf-8") == 0)
        return 0;
    if (strcmp(mime, "text/plain") == 0)
        return 1;
    if (strcmp(mime, "UTF8_STRING") == 0)
        return 2;
    if (strcmp(mime, "STRING") == 0)
        return 3;
    return 100;
}

static void data_offer_offer(void *data, struct wl_data_offer *offer,
                             const char *mime_type)
{
    struct app *app = data;
    int new_rank;
    int old_rank;
    (void)offer;

    new_rank = clipboard_mime_rank(mime_type);
    old_rank = app->clipboard_offer_mime[0]
                   ? clipboard_mime_rank(app->clipboard_offer_mime)
                   : 100;
    if (new_rank < old_rank)
        checked_copy(app->clipboard_offer_mime,
                     sizeof(app->clipboard_offer_mime), mime_type,
                     "clipboard mime");
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = data_offer_offer,
};

static void read_clipboard_offer(struct app *app)
{
    int fds[2];
    char buf[MAX_TEXT];
    size_t off = 0;

    if (!app->clipboard_offer || !app->clipboard_offer_mime[0])
        return;
    if (pipe(fds) < 0)
        fatal("clipboard pipe failed: %s", strerror(errno));

    wl_data_offer_receive(app->clipboard_offer, app->clipboard_offer_mime,
                          fds[1]);
    close(fds[1]);
    checked_flush(app->display);

    for (;;) {
        struct pollfd pfd = {
            .fd = fds[0],
            .events = POLLIN | POLLHUP | POLLERR,
        };
        int pr = poll(&pfd, 1, 5000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fatal("clipboard poll failed: %s", strerror(errno));
        }
        if (pr == 0)
            fatal("clipboard receive timed out");
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fds[0], buf + off, sizeof(buf) - off - 1);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fatal("clipboard read failed: %s", strerror(errno));
            }
            if (n == 0)
                break;
            off += (size_t)n;
            if (off >= sizeof(buf) - 1)
                fatal("clipboard text too large for debug app");
        }
    }
    close(fds[0]);
    buf[off] = '\0';
    debug_emit("CLIPBOARD_PASTE", buf);
}

static void data_device_data_offer(void *data, struct wl_data_device *device,
                                   struct wl_data_offer *offer)
{
    struct app *app = data;
    (void)device;
    if (app->clipboard_offer)
        wl_data_offer_destroy(app->clipboard_offer);
    app->clipboard_offer = offer;
    app->clipboard_offer_mime[0] = '\0';
    wl_data_offer_add_listener(offer, &data_offer_listener, app);
}

static void data_device_enter(void *data, struct wl_data_device *device,
                              uint32_t serial, struct wl_surface *surface,
                              wl_fixed_t x, wl_fixed_t y,
                              struct wl_data_offer *offer)
{
    (void)data;
    (void)device;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    (void)offer;
}

static void data_device_leave(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_motion(void *data, struct wl_data_device *device,
                               uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static void data_device_drop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_selection(void *data, struct wl_data_device *device,
                                  struct wl_data_offer *offer)
{
    struct app *app = data;
    (void)device;
    if (!offer) {
        debug_emit("CLIPBOARD_SELECTION", "");
        return;
    }
    if (!app->paste_clipboard_on_selection)
        return;
    if (offer == app->clipboard_offer)
        read_clipboard_offer(app);
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_data_offer,
    .enter = data_device_enter,
    .leave = data_device_leave,
    .motion = data_device_motion,
    .drop = data_device_drop,
    .selection = data_device_selection,
};

static void data_source_target(void *data, struct wl_data_source *source,
                               const char *mime_type)
{
    (void)data;
    (void)source;
    (void)mime_type;
}

static void data_source_send(void *data, struct wl_data_source *source,
                             const char *mime_type, int32_t fd)
{
    struct app *app = data;
    (void)source;
    (void)mime_type;
    debug_emit("CLIPBOARD_SEND", mime_type);

    if (app->clipboard_generated_bytes > 0) {
        char chunk[4096];
        size_t remaining = app->clipboard_generated_bytes;
        memset(chunk, 'x', sizeof(chunk));
        while (remaining > 0) {
            size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            ssize_t written = write(fd, chunk, n);
            if (written < 0) {
                debug_emit("CLIPBOARD_SEND_ERROR", strerror(errno));
                break;
            }
            remaining -= (size_t)written;
        }
    } else {
        size_t len = strlen(app->clipboard_copy_text);
        if (write(fd, app->clipboard_copy_text, len) < 0)
            debug_emit("CLIPBOARD_SEND_ERROR", strerror(errno));
    }

    if (app->clipboard_leave_fd_open) {
        if (app->held_clipboard_fd >= 0)
            close(app->held_clipboard_fd);
        app->held_clipboard_fd = fd;
    } else {
        close(fd);
    }
}

static void data_source_cancelled(void *data, struct wl_data_source *source)
{
    struct app *app = data;
    if (app->clipboard_source == source)
        app->clipboard_source = NULL;
    wl_data_source_destroy(source);
}

static void data_source_dnd_drop_performed(void *data,
                                           struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void data_source_dnd_finished(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void data_source_action(void *data, struct wl_data_source *source,
                               uint32_t dnd_action)
{
    (void)data;
    (void)source;
    (void)dnd_action;
}

static const struct wl_data_source_listener data_source_listener = {
    .target = data_source_target,
    .send = data_source_send,
    .cancelled = data_source_cancelled,
    .dnd_drop_performed = data_source_dnd_drop_performed,
    .dnd_finished = data_source_dnd_finished,
    .action = data_source_action,
};

static struct wl_data_source *create_clipboard_source(struct app *app,
                                                      int with_save_targets)
{
    struct wl_data_source *source =
        wl_data_device_manager_create_data_source(app->data_device_manager);
    require_true(source != NULL, "create_data_source returned NULL");
    wl_data_source_add_listener(source, &data_source_listener, app);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "text/plain");
    wl_data_source_offer(source, "UTF8_STRING");
    wl_data_source_offer(source, "STRING");
    if (with_save_targets)
        wl_data_source_offer(source, "SAVE_TARGETS");
    return source;
}

static void maybe_set_clipboard_selection(struct app *app)
{
    if (!app->copy_clipboard_on_focus || app->clipboard_set ||
        !app->data_device_manager || !app->data_device ||
        app->keyboard_enter_serial == 0)
        return;

    if (app->clipboard_double_set) {
        /* GTK3-style clipboard-manager dance (Firefox, VTE terminals):
         * set the selection, then immediately replace it with a second
         * source re-announcing the same targets plus SAVE_TARGETS. The
         * compositor must mirror the final selection of the burst. */
        struct wl_data_source *first = create_clipboard_source(app, 0);
        wl_data_device_set_selection(app->data_device, first,
                                     app->keyboard_enter_serial);
        app->clipboard_source = create_clipboard_source(app, 1);
    } else {
        app->clipboard_source = create_clipboard_source(app, 0);
    }
    wl_data_device_set_selection(app->data_device, app->clipboard_source,
                                 app->keyboard_enter_serial);
    checked_flush(app->display);
    app->clipboard_set = 1;
    debug_emit("CLIPBOARD_SET", app->clipboard_copy_text);
}

/* --- Drawing ------------------------------------------------------------ */

static int create_shm_fd(size_t size)
{
    int fd = (int)syscall(SYS_memfd_create, "tawc-wayland-debug", MFD_CLOEXEC);
    if (fd < 0) {
        char tmpl[] = "/tmp/tawc-wayland-debug-XXXXXX";
        fd = mkstemp(tmpl);
        if (fd >= 0)
            unlink(tmpl);
    }
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct shm_buffer *buf = data;
    (void)buffer;

    if (buf->pool) {
        struct main_shm_pool *pool = buf->pool;
        buf->busy = 0;
        if (pool->retired) {
            if (buf->buffer) {
                wl_buffer_destroy(buf->buffer);
                buf->buffer = NULL;
                pool->live_buffers--;
            }
            if (pool->live_buffers == 0) {
                munmap(pool->data, pool->size);
                free(pool);
            }
            return;
        }
        if (pool->app && pool->app->redraw_pending) {
            pool->app->redraw_pending = 0;
            request_redraw(pool->app);
        }
        return;
    }

    wl_buffer_destroy(buf->buffer);
    munmap(buf->data, buf->size);
    free(buf);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void free_available_retired_pool_buffers(struct main_shm_pool *pool)
{
    for (int i = 0; i < MAIN_BUFFER_COUNT; i++) {
        struct shm_buffer *buf = &pool->buffers[i];
        if (buf->buffer && !buf->busy) {
            wl_buffer_destroy(buf->buffer);
            buf->buffer = NULL;
            pool->live_buffers--;
        }
    }
    if (pool->live_buffers == 0) {
        munmap(pool->data, pool->size);
        free(pool);
    }
}

static void retire_main_shm_pool(struct main_shm_pool *pool)
{
    if (!pool)
        return;
    pool->retired = 1;
    pool->app = NULL;
    free_available_retired_pool_buffers(pool);
}

static struct main_shm_pool *create_main_shm_pool(struct app *app, int width,
                                                  int height)
{
    struct main_shm_pool *pool = calloc(1, sizeof(*pool));
    require_true(pool != NULL, "calloc main_shm_pool failed");

    pool->app = app;
    pool->width = width;
    pool->height = height;
    pool->stride = width * 4;
    size_t buffer_size = (size_t)pool->stride * height;
    pool->size = buffer_size * MAIN_BUFFER_COUNT;

    int fd = create_shm_fd(pool->size);
    if (fd < 0)
        fatal("create main shm failed: %s", strerror(errno));

    pool->data = mmap(NULL, pool->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
    if (pool->data == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        free(pool);
        fatal("main shm mmap failed: %s", strerror(saved_errno));
    }

    struct wl_shm_pool *wl_pool =
        wl_shm_create_pool(app->shm, fd, (int32_t)pool->size);
    require_true(wl_pool != NULL, "wl_shm_create_pool main returned NULL");

    for (int i = 0; i < MAIN_BUFFER_COUNT; i++) {
        struct shm_buffer *buf = &pool->buffers[i];
        size_t offset = buffer_size * (size_t)i;
        buf->pool = pool;
        buf->data = (char *)pool->data + offset;
        buf->size = buffer_size;
        buf->index = i;
        buf->buffer = wl_shm_pool_create_buffer(
            wl_pool, (int32_t)offset, width, height, pool->stride,
            WL_SHM_FORMAT_ARGB8888);
        require_true(buf->buffer != NULL,
                     "wl_shm_pool_create_buffer main returned NULL");
        wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
        pool->live_buffers++;
    }

    wl_shm_pool_destroy(wl_pool);
    close(fd);
    return pool;
}

static void ensure_main_shm_pool(struct app *app, int width, int height)
{
    if (app->main_pool && app->main_pool->width == width &&
        app->main_pool->height == height)
        return;

    retire_main_shm_pool(app->main_pool);
    app->main_pool = create_main_shm_pool(app, width, height);
    app->main_next_buffer = 0;
    app->redraw_pending = 0;
}

static struct shm_buffer *next_main_buffer(struct app *app)
{
    for (int n = 0; n < MAIN_BUFFER_COUNT; n++) {
        int i = (app->main_next_buffer + n) % MAIN_BUFFER_COUNT;
        struct shm_buffer *buf = &app->main_pool->buffers[i];
        if (!buf->busy && buf->buffer) {
            app->main_next_buffer = (i + 1) % MAIN_BUFFER_COUNT;
            return buf;
        }
    }
    return NULL;
}

static void draw_display_text(struct app *app, cairo_t *cr)
{
    char visible[MAX_TEXT * 2];
    size_t o = 0;
    for (size_t i = 0; i < app->text_len && o + 3 < sizeof(visible); i++) {
        if (i == app->cursor && app->preedit[0]) {
            const char *p = app->preedit;
            while (*p && o + 1 < sizeof(visible))
                visible[o++] = *p++;
        }
        if (app->text[i] == '\n') {
            visible[o++] = '\\';
            visible[o++] = 'n';
        } else {
            visible[o++] = app->text[i];
        }
    }
    if (app->cursor == app->text_len && app->preedit[0]) {
        const char *p = app->preedit;
        while (*p && o + 1 < sizeof(visible))
            visible[o++] = *p++;
    }
    visible[o] = '\0';

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, FONT_SIZE);

    cairo_set_source_rgb(cr, 0.07, 0.08, 0.09);
    cairo_move_to(cr, TEXT_X, TEXT_Y);
    cairo_show_text(cr, visible);

    char before[MAX_TEXT * 2];
    o = 0;
    for (size_t i = 0; i < app->cursor && o + 3 < sizeof(before); i++) {
        if (app->text[i] == '\n') {
            before[o++] = '\\';
            before[o++] = 'n';
        } else {
            before[o++] = app->text[i];
        }
    }
    before[o] = '\0';

    cairo_text_extents_t ext;
    cairo_text_extents(cr, before, &ext);
    double cx = TEXT_X + ext.x_advance;
    cairo_set_source_rgb(cr, 0.0, 0.35, 0.85);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, cx, TEXT_Y - FONT_SIZE);
    cairo_line_to(cr, cx, TEXT_Y + 8.0);
    cairo_stroke(cr);

    if (app->preedit[0]) {
        cairo_text_extents_t pre;
        cairo_text_extents(cr, app->preedit, &pre);
        cairo_set_source_rgb(cr, 0.85, 0.15, 0.10);
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, cx, TEXT_Y + 14.0);
        cairo_line_to(cr, cx + pre.x_advance, TEXT_Y + 14.0);
        cairo_stroke(cr);
    }
}

static int active_touch_count(struct app *app)
{
    int count = 0;
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (app->touches[i].active)
            count++;
    }
    return count;
}

static struct touch_point *find_touch(struct app *app, int32_t id)
{
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (app->touches[i].seen && app->touches[i].id == id)
            return &app->touches[i];
    }
    return NULL;
}

static struct touch_point *alloc_touch(struct app *app, int32_t id)
{
    struct touch_point *point = find_touch(app, id);
    if (point)
        return point;
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (!app->touches[i].active) {
            app->touches[i].seen = 1;
            app->touches[i].id = id;
            return &app->touches[i];
        }
    }
    fatal("too many touch slots");
    return NULL;
}

static void emit_touch_event(struct app *app, const char *tag, int32_t id,
                             double x, double y)
{
    char buf[96];
    checked_snprintf(buf, sizeof(buf), "%d:%.1f:%.1f:%d", id, x, y,
                     active_touch_count(app));
    debug_emit(tag, buf);
}

static const char *surface_label_for_touch(struct app *app,
                                           struct wl_surface *surface)
{
    if (surface == app->surface)
        return "toplevel";
    if (surface == app->child_surface) {
        if (app->scene_kind == SCENE_SUBSURFACE)
            return "subsurface";
        if (app->scene_kind == SCENE_POPUP ||
            app->scene_kind == SCENE_POPUP_SWITCH)
            return "popup";
    }
    if (surface == app->second_child_surface)
        return "popup2";
    return NULL;
}

static void emit_scene_touch_event(struct app *app, const char *tag,
                                   const char *label, int32_t id,
                                   double x, double y)
{
    char buf[128];
    checked_snprintf(buf, sizeof(buf), "%s:%d:%.1f:%.1f:%d", label, id, x, y,
                     active_touch_count(app));
    debug_emit(tag, buf);
}

static void emit_popup_layout(struct app *app)
{
    char buf[192];
    checked_snprintf(
        buf, sizeof(buf), "%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d",
        app->child_x, app->child_y, POPUP_SHADOW, CHILD_W, CHILD_H,
        app->popup_configure_x, app->popup_configure_y,
        app->popup_configure_w, app->popup_configure_h,
        POPUP_PARENT_GEOM_X, POPUP_PARENT_GEOM_Y);
    debug_emit("POPUP_LAYOUT", buf);
}

static void child_ready_done(void *data, struct wl_callback *callback,
                             uint32_t time)
{
    struct app *app = data;
    (void)time;
    require_true(callback == app->child_ready_callback ||
                 callback == app->second_child_ready_callback,
                 "ready callback for unexpected object");
    if (callback == app->second_child_ready_callback) {
        wl_callback_destroy(callback);
        app->second_child_ready_callback = NULL;
        debug_emit("SURFACE_READY", "popup2");
        return;
    }

    wl_callback_destroy(callback);
    app->child_ready_callback = NULL;
    app->scene_child_ready = 1;
    if (app->scene_kind == SCENE_POPUP ||
        app->scene_kind == SCENE_POPUP_SWITCH)
        emit_popup_layout(app);
    debug_emit("SURFACE_READY",
               (app->scene_kind == SCENE_POPUP ||
                app->scene_kind == SCENE_POPUP_SWITCH)
                   ? "popup" : "subsurface");
    if (!app->ready_emitted) {
        app->ready_emitted = 1;
        debug_emit("READY", NULL);
    }
}

static const struct wl_callback_listener child_ready_listener = {
    .done = child_ready_done,
};

static void draw_touch_debug(struct app *app, cairo_t *cr)
{
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    cairo_set_source_rgb(cr, 0.95, 0.96, 0.95);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.82, 0.84, 0.84);
    cairo_set_line_width(cr, 1.0);
    for (int x = 0; x < app->win_w; x += 80) {
        cairo_move_to(cr, x + 0.5, 0);
        cairo_line_to(cr, x + 0.5, app->win_h);
    }
    for (int y = 0; y < app->win_h; y += 80) {
        cairo_move_to(cr, 0, y + 0.5);
        cairo_line_to(cr, app->win_w, y + 0.5);
    }
    cairo_stroke(cr);

    cairo_set_font_size(cr, 18.0);
    cairo_set_source_rgb(cr, 0.08, 0.09, 0.10);
    cairo_move_to(cr, 24.0, 34.0);
    cairo_show_text(cr, "wayland-debug-app touch");

    for (int i = 0; i < MAX_TOUCHES; i++) {
        struct touch_point *point = &app->touches[i];
        if (!point->active)
            continue;

        double hue = (double)((point->id % 6 + 6) % 6);
        double r = hue == 0 || hue == 5 ? 0.95 : 0.15;
        double g = hue >= 1 && hue <= 3 ? 0.65 : 0.25;
        double b = hue >= 3 ? 0.95 : 0.20;

        cairo_set_source_rgba(cr, r, g, b, 0.30);
        cairo_arc(cr, point->x, point->y, 38.0, 0.0, 6.283185307179586);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_set_line_width(cr, 4.0);
        cairo_stroke(cr);

        char label[32];
        checked_snprintf(label, sizeof(label), "%d", point->id);
        cairo_set_font_size(cr, 24.0);
        cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        cairo_move_to(cr, point->x - 7.0, point->y + 8.0);
        cairo_show_text(cr, label);
    }
}

static void draw_render_pattern(struct app *app, cairo_t *cr)
{
    const double inset = 96.0;
    const double size = 80.0;
    double right = app->win_w - inset - size;
    double bottom = app->win_h - inset - size;

    cairo_set_source_rgb(cr, 0.12, 0.10, 0.14);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.92, 0.12, 0.10);
    cairo_rectangle(cr, inset, inset, size, size);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.10, 0.72, 0.20);
    cairo_rectangle(cr, right, inset, size, size);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.15, 0.25, 0.92);
    cairo_rectangle(cr, inset, bottom, size, size);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.92, 0.84, 0.12);
    cairo_rectangle(cr, right, bottom, size, size);
    cairo_fill(cr);
}

static void attach_labeled_buffer(struct app *app, struct wl_surface *surface,
                                  int width, int height, const char *label,
                                  double r, double g, double b)
{
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = create_shm_fd(size);
    if (fd < 0)
        fatal("create child shm failed: %s", strerror(errno));

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        fatal("child mmap failed: %s", strerror(saved_errno));
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    struct shm_buffer *buf = calloc(1, sizeof(*buf));
    require_true(pool != NULL, "wl_shm_create_pool child returned NULL");
    require_true(buf != NULL, "calloc child shm_buffer failed");
    buf->data = data;
    buf->size = size;
    buf->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_ARGB8888);
    require_true(buf->buffer != NULL, "wl_shm_pool_create_buffer child returned NULL");
    wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
    wl_shm_pool_destroy(pool);
    close(fd);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t *cr = cairo_create(cs);
    require_true(cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS,
                 "child cairo surface failed: %s",
                 cairo_status_to_string(cairo_surface_status(cs)));
    require_true(cairo_status(cr) == CAIRO_STATUS_SUCCESS,
                 "child cairo context failed: %s",
                 cairo_status_to_string(cairo_status(cr)));

    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.06);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, 2.0, 2.0, width - 4.0, height - 4.0);
    cairo_stroke(cr);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22.0);
    cairo_move_to(cr, 18.0, 42.0);
    cairo_show_text(cr, label);

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    if (app->scene_kind != SCENE_NONE && surface == app->child_surface &&
        !app->scene_child_ready && !app->child_ready_callback) {
        app->child_ready_callback = wl_surface_frame(surface);
        require_true(app->child_ready_callback != NULL,
                     "wl_surface_frame child returned NULL");
        wl_callback_add_listener(app->child_ready_callback,
                                 &child_ready_listener, app);
    }
    if (surface == app->second_child_surface &&
        !app->second_child_ready_callback) {
        app->second_child_ready_callback = wl_surface_frame(surface);
        require_true(app->second_child_ready_callback != NULL,
                     "wl_surface_frame second child returned NULL");
        wl_callback_add_listener(app->second_child_ready_callback,
                                 &child_ready_listener, app);
    }

    wl_surface_attach(surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

static void request_redraw(struct app *app)
{
    if (!app->configured || !app->surface || !app->shm)
        return;

    int width = app->win_w > 0 ? app->win_w : WIN_W;
    int height = app->win_h > 0 ? app->win_h : WIN_H;
    ensure_main_shm_pool(app, width, height);
    struct shm_buffer *buf = next_main_buffer(app);
    if (!buf) {
        app->redraw_pending = 1;
        return;
    }

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, width, height, app->main_pool->stride);
    cairo_t *cr = cairo_create(cs);
    require_true(cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS,
                 "cairo surface failed: %s",
                 cairo_status_to_string(cairo_surface_status(cs)));
    require_true(cairo_status(cr) == CAIRO_STATUS_SUCCESS,
                 "cairo context failed: %s",
                 cairo_status_to_string(cairo_status(cr)));

    if (app->render_pattern) {
        draw_render_pattern(app, cr);
    } else if (app->touch_debug) {
        draw_touch_debug(app, cr);
    } else {
        cairo_set_source_rgb(cr, 0.96, 0.96, 0.94);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.82, 0.84, 0.86);
        cairo_rectangle(cr, 16.0, 52.0, width - 32.0, 72.0);
        cairo_fill(cr);
        draw_display_text(app, cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    buf->busy = 1;
    wl_surface_attach(app->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, width, height);
    wl_surface_commit(app->surface);
    checked_flush(app->display);

    if (!app->ready_emitted &&
        (app->scene_kind == SCENE_NONE ||
         app->scene_kind == SCENE_POPUP_SWITCH)) {
        app->ready_emitted = 1;
        debug_emit("READY", NULL);
    }
}

/* --- Protocol listeners ------------------------------------------------- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void fractional_scale_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *fractional_scale,
    uint32_t preferred_scale)
{
    struct app *app = data;
    char buf[32];
    (void)fractional_scale;
    app->last_preferred_scale = preferred_scale;
    checked_snprintf(buf, sizeof(buf), "%.2f", (double)preferred_scale / 120.0);
    debug_emit("SCALE_CHANGED", buf);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_preferred_scale,
};

static void output_geometry(
    void *data,
    struct wl_output *output,
    int32_t x,
    int32_t y,
    int32_t physical_width,
    int32_t physical_height,
    int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(
    void *data,
    struct wl_output *output,
    uint32_t flags,
    int32_t width,
    int32_t height,
    int32_t refresh)
{
    (void)data;
    (void)output;
    (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT)
        debug_emit_i32_pair("OUTPUT_MODE", width, height);
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data;
    (void)output;
    debug_emit_u32("OUTPUT_SCALE", (uint32_t)factor);
}

static void output_name(void *data, struct wl_output *output, const char *name)
{
    (void)data;
    (void)output;
    (void)name;
}

static void output_description(
    void *data,
    struct wl_output *output,
    const char *description)
{
    (void)data;
    (void)output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void decoration_configure(
    void *data,
    struct zxdg_toplevel_decoration_v1 *decoration,
    uint32_t mode)
{
    (void)data;
    (void)decoration;
    const char *name = "unknown";
    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
        name = "client-side";
    else if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
        name = "server-side";
    debug_emit("DECORATION_MODE", name);
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void create_scene_child(struct app *app);

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    if (xdg_surface == app->child_xdg_surface) {
        xdg_surface_set_window_geometry(app->child_xdg_surface, POPUP_SHADOW,
                                        POPUP_SHADOW, CHILD_W, CHILD_H);
        attach_labeled_buffer(app, app->child_surface,
                              CHILD_W + POPUP_SHADOW * 2,
                              CHILD_H + POPUP_SHADOW * 2, "popup",
                              0.98, 0.72, 0.22);
        checked_flush(app->display);
        return;
    }
    if (xdg_surface == app->second_child_xdg_surface) {
        xdg_surface_set_window_geometry(app->second_child_xdg_surface,
                                        POPUP_SHADOW, POPUP_SHADOW,
                                        CHILD_W, CHILD_H);
        attach_labeled_buffer(app, app->second_child_surface,
                              CHILD_W + POPUP_SHADOW * 2,
                              CHILD_H + POPUP_SHADOW * 2, "popup2",
                              0.42, 0.72, 0.96);
        checked_flush(app->display);
        return;
    }

    require_true(xdg_surface == app->xdg_surface,
                 "configure for unexpected xdg_surface %p",
                 (void *)xdg_surface);
    app->configured = 1;
    if (app->scene_kind == SCENE_POPUP ||
        app->scene_kind == SCENE_POPUP_SWITCH) {
        xdg_surface_set_window_geometry(
            app->xdg_surface, POPUP_PARENT_GEOM_X, POPUP_PARENT_GEOM_Y,
            app->win_w - POPUP_PARENT_GEOM_X - POPUP_PARENT_GEOM_R,
            app->win_h - POPUP_PARENT_GEOM_Y - POPUP_PARENT_GEOM_B);
    }
    request_redraw(app);
    create_scene_child(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    struct app *app = data;
    uint32_t *state;
    int maximized = 0;
    int fullscreen = 0;
    int activated = 0;
    (void)toplevel;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            maximized = 1;
        else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            fullscreen = 1;
        else if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
            activated = 1;
    }
    debug_emit("CONFIGURE_STATE",
               fullscreen ? "fullscreen" : (maximized ? "maximized" : "other"));
    debug_emit_u32("CONFIGURE_ACTIVE", activated ? 1 : 0);
    debug_emit_i32_pair("CONFIGURE_SIZE", width, height);
    if (width > 0 && height > 0) {
        app->win_w = width;
        app->win_h = height;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;
    (void)toplevel;
    app->running = 0;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height)
{
    (void)toplevel;
    debug_emit_i32_pair("CONFIGURE_BOUNDS", width, height);
    (void)data;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities)
{
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void popup_configure(void *data, struct xdg_popup *popup, int32_t x,
                            int32_t y, int32_t width, int32_t height)
{
    struct app *app = data;
    char buf[96];
    (void)popup;
    app->popup_configure_x = x;
    app->popup_configure_y = y;
    app->popup_configure_w = width;
    app->popup_configure_h = height;
    checked_snprintf(buf, sizeof(buf), "%d:%d:%d:%d", x, y, width, height);
    debug_emit("POPUP_CONFIGURE", buf);
}

static void popup_done(void *data, struct xdg_popup *popup)
{
    (void)data;
    (void)popup;
    debug_emit("POPUP_DONE", NULL);
}

static void popup_repositioned(void *data, struct xdg_popup *popup,
                               uint32_t token)
{
    (void)data;
    (void)popup;
    (void)token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static int clamp_i32(int value, int min, int max)
{
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

static void compute_child_position(struct app *app)
{
    int x = (int)(app->win_w * TAP_FRAC_X) - CHILD_W / 2;
    int y = (int)(app->win_h * TAP_FRAC_Y) - CHILD_H / 2;
    app->child_x = clamp_i32(x, 0, app->win_w > CHILD_W ? app->win_w - CHILD_W : 0);
    app->child_y = clamp_i32(y, 0, app->win_h > CHILD_H ? app->win_h - CHILD_H : 0);
}

static void create_scene_child(struct app *app)
{
    if (app->scene_kind == SCENE_NONE ||
        app->scene_kind == SCENE_POPUP_SWITCH ||
        app->scene_child_created || !app->configured)
        return;

    compute_child_position(app);
    app->child_surface = wl_compositor_create_surface(app->compositor);
    require_true(app->child_surface != NULL,
                 "wl_compositor_create_surface child returned NULL");
    if (app->scene_child_input_empty) {
        struct wl_region *region = wl_compositor_create_region(app->compositor);
        require_true(region != NULL,
                     "wl_compositor_create_region child input returned NULL");
        wl_surface_set_input_region(app->child_surface, region);
        wl_region_destroy(region);
    }

    if (app->scene_kind == SCENE_SUBSURFACE) {
        app->child_subsurface = wl_subcompositor_get_subsurface(
            app->subcompositor, app->child_surface, app->surface);
        require_true(app->child_subsurface != NULL,
                     "wl_subcompositor_get_subsurface returned NULL");
        wl_subsurface_set_position(app->child_subsurface, app->child_x,
                                   app->child_y);
        wl_subsurface_set_desync(app->child_subsurface);
        attach_labeled_buffer(app, app->child_surface, CHILD_W, CHILD_H,
                              "subsurface", 0.35, 0.86, 0.62);
        wl_surface_commit(app->surface);
        checked_flush(app->display);
    } else if (app->scene_kind == SCENE_POPUP) {
        app->child_xdg_surface =
            xdg_wm_base_get_xdg_surface(app->wm_base, app->child_surface);
        require_true(app->child_xdg_surface != NULL,
                     "xdg_wm_base_get_xdg_surface popup returned NULL");
        xdg_surface_add_listener(app->child_xdg_surface,
                                 &xdg_surface_listener, app);

        struct xdg_positioner *positioner =
            xdg_wm_base_create_positioner(app->wm_base);
        require_true(positioner != NULL,
                     "xdg_wm_base_create_positioner returned NULL");
        xdg_positioner_set_size(positioner, CHILD_W, CHILD_H);
        xdg_positioner_set_anchor_rect(
            positioner, app->child_x - POPUP_PARENT_GEOM_X,
            app->child_y - POPUP_PARENT_GEOM_Y, 1, 1);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(positioner,
                                   XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE);
        app->child_popup = xdg_surface_get_popup(
            app->child_xdg_surface, app->xdg_surface, positioner);
        require_true(app->child_popup != NULL,
                     "xdg_surface_get_popup returned NULL");
        xdg_popup_add_listener(app->child_popup, &popup_listener, app);
        xdg_surface_set_window_geometry(app->child_xdg_surface, POPUP_SHADOW,
                                        POPUP_SHADOW, CHILD_W, CHILD_H);
        xdg_positioner_destroy(positioner);
        wl_surface_commit(app->child_surface);
        checked_flush(app->display);
    }

    app->scene_child_created = 1;
}

static void create_switch_popup(struct app *app, int second, uint32_t serial)
{
    int x = (int)(app->win_w * (second ? 0.65 : 0.30)) - CHILD_W / 2;
    int y = (int)(app->win_h * 0.25) - CHILD_H / 2;
    x = clamp_i32(x, 0, app->win_w > CHILD_W ? app->win_w - CHILD_W : 0);
    y = clamp_i32(y, 0, app->win_h > CHILD_H ? app->win_h - CHILD_H : 0);

    struct wl_surface **surface =
        second ? &app->second_child_surface : &app->child_surface;
    struct xdg_surface **xdg_surface =
        second ? &app->second_child_xdg_surface : &app->child_xdg_surface;
    struct xdg_popup **popup =
        second ? &app->second_child_popup : &app->child_popup;

    *surface = wl_compositor_create_surface(app->compositor);
    require_true(*surface != NULL, "wl_compositor_create_surface switch popup");
    *xdg_surface = xdg_wm_base_get_xdg_surface(app->wm_base, *surface);
    require_true(*xdg_surface != NULL, "xdg surface switch popup");
    xdg_surface_add_listener(*xdg_surface, &xdg_surface_listener, app);

    struct xdg_positioner *positioner =
        xdg_wm_base_create_positioner(app->wm_base);
    require_true(positioner != NULL, "switch popup positioner");
    xdg_positioner_set_size(positioner, CHILD_W, CHILD_H);
    xdg_positioner_set_anchor_rect(positioner, x - POPUP_PARENT_GEOM_X,
                                   y - POPUP_PARENT_GEOM_Y, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(positioner,
                               XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(
        positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE);
    *popup = xdg_surface_get_popup(*xdg_surface, app->xdg_surface, positioner);
    require_true(*popup != NULL, "xdg_surface_get_popup switch popup");
    xdg_popup_add_listener(*popup, &popup_listener, app);
    xdg_popup_grab(*popup, app->seat, serial);
    xdg_surface_set_window_geometry(*xdg_surface, POPUP_SHADOW,
                                    POPUP_SHADOW, CHILD_W, CHILD_H);
    xdg_positioner_destroy(positioner);
    wl_surface_commit(*surface);
    debug_emit("POPUP_OPEN_REQUEST", second ? "second" : "first");
    checked_flush(app->display);
}

static void text_input_enter(void *data, struct zwp_text_input_v3 *text_input,
                             struct wl_surface *surface)
{
    struct app *app = data;
    (void)text_input;
    require_true(surface == app->surface,
                 "text-input enter for unexpected surface %p", (void *)surface);
    require_true(!app->text_input_enabled, "duplicate text-input enter");
    app->text_input_enabled = 1;
    zwp_text_input_v3_enable(app->text_input);
    zwp_text_input_v3_set_content_type(
        app->text_input, app->content_hint, app->content_purpose);
    zwp_text_input_v3_set_cursor_rectangle(app->text_input, (int32_t)TEXT_X,
                                           (int32_t)(TEXT_Y - FONT_SIZE),
                                           2, (int32_t)(FONT_SIZE + 8.0));
    if (app->provide_surrounding)
        zwp_text_input_v3_set_surrounding_text(app->text_input, app->text,
                                               (int32_t)app->cursor,
                                               (int32_t)app->cursor);
    zwp_text_input_v3_commit(app->text_input);
    app->text_input_commit_serial++;
    checked_flush(app->display);
}

static void text_input_leave(void *data, struct zwp_text_input_v3 *text_input,
                             struct wl_surface *surface)
{
    struct app *app = data;
    require_true(surface == app->surface,
                 "text-input leave for unexpected surface %p", (void *)surface);
    require_true(app->text_input_enabled, "text-input leave before enter");
    require_true(!app->pending_text_input.has_preedit &&
                     !app->pending_text_input.has_commit &&
                     !app->pending_text_input.has_delete,
                 "text-input leave with uncommitted pending transaction");
    app->text_input_enabled = 0;
    debug_emit("TEXT_INPUT_LEAVE", NULL);
    zwp_text_input_v3_disable(text_input);
    zwp_text_input_v3_commit(text_input);
    app->text_input_commit_serial++;
    checked_flush(app->display);
}

static void text_input_preedit_string(void *data,
                                      struct zwp_text_input_v3 *text_input,
                                      const char *text, int32_t cursor_begin,
                                      int32_t cursor_end)
{
    struct app *app = data;
    struct text_input_pending *pending = &app->pending_text_input;
    (void)text_input;
    (void)cursor_begin;
    (void)cursor_end;
    require_true(app->text_input_enabled, "preedit_string before text-input enter");
    checked_copy(pending->preedit, sizeof(pending->preedit),
                 text ? text : "", "preedit_string");
    pending->has_preedit = 1;
}

static void text_input_commit_string(void *data,
                                     struct zwp_text_input_v3 *text_input,
                                     const char *text)
{
    struct app *app = data;
    struct text_input_pending *pending = &app->pending_text_input;
    (void)text_input;
    require_true(app->text_input_enabled, "commit_string before text-input enter");
    checked_copy(pending->commit, sizeof(pending->commit),
                 text ? text : "", "commit_string");
    pending->has_commit = 1;
}

static void text_input_delete_surrounding_text(
    void *data, struct zwp_text_input_v3 *text_input, uint32_t before_length,
    uint32_t after_length)
{
    struct app *app = data;
    struct text_input_pending *pending = &app->pending_text_input;
    (void)text_input;
    require_true(app->text_input_enabled,
                 "delete_surrounding_text before text-input enter");
    pending->before_length = before_length;
    pending->after_length = after_length;
    pending->has_delete = 1;
}

static void text_input_done(void *data, struct zwp_text_input_v3 *text_input,
                            uint32_t serial)
{
    struct app *app = data;
    (void)text_input;
    require_true(app->text_input_enabled, "done before text-input enter");
    require_true(serial == app->text_input_commit_serial,
                 "done serial mismatch: got %u expected %u", serial,
                 app->text_input_commit_serial);
    if (app->editable)
        apply_text_input_transaction(app);
    else
        apply_surroundingless_text_input_transaction(app);
    debug_emit("DONE", NULL);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit_string,
    .commit_string = text_input_commit_string,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .done = text_input_done,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    if (fd >= 0)
        close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    struct app *app = data;
    const char *label = surface_label_for_touch(app, surface);
    (void)keyboard;
    (void)keys;
    require_true(label != NULL,
                 "keyboard enter for unexpected surface %p", (void *)surface);
    require_true(strcmp(label, "subsurface") != 0,
                 "protocol violation: wl_subsurface received keyboard focus");
    app->keyboard_enter_serial = serial;
    debug_emit("KEYBOARD_ENTER", label);
    maybe_set_clipboard_selection(app);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    struct app *app = data;
    const char *label = surface ? surface_label_for_touch(app, surface) : "nil";
    (void)keyboard;
    (void)serial;
    require_true(label == NULL || strcmp(label, "subsurface") != 0,
                 "protocol violation: wl_subsurface lost keyboard focus");
    debug_emit("KEYBOARD_LEAVE", label ? label : "unknown");
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    struct app *app = data;
    char buf[32];
    const char *payload = NULL;
    (void)keyboard;
    (void)serial;
    (void)time;
    if (key == KEY_BACKSPACE) {
        payload = "BackSpace";
    } else if (key == KEY_DELETE) {
        payload = "Delete";
    } else if (key == KEY_ENTER) {
        payload = "Return";
    } else if (key == KEY_TAB) {
        payload = "Tab";
    } else {
        checked_snprintf(buf, sizeof(buf), "%u", key);
        payload = buf;
    }

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        debug_emit("KEY_RELEASE", payload);
        return;
    }
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    if (key == KEY_BACKSPACE) {
        debug_emit("KEY", payload);
        if (!app->editable)
            return;
        if (app->cursor > 0) {
            size_t start = prev_char_start(app->text, app->cursor);
            delete_range(app, start, app->cursor);
            changed_by_input_method(app);
        }
    } else if (key == KEY_DELETE) {
        debug_emit("KEY", payload);
        if (!app->editable)
            return;
        if (app->cursor < app->text_len) {
            size_t end = next_char_end(app->text, app->text_len, app->cursor);
            delete_range(app, app->cursor, end);
            changed_by_input_method(app);
        }
    } else if (key == KEY_ENTER) {
        debug_emit("KEY", payload);
        if (!app->editable)
            return;
        insert_text(app, "\n");
        changed_by_input_method(app);
    } else if (key == KEY_TAB) {
        debug_emit("KEY", payload);
        if (!app->editable)
            return;
        insert_text(app, "\t");
        changed_by_input_method(app);
    } else {
        debug_emit("KEY", payload);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    char buf[64];
    checked_snprintf(buf, sizeof(buf), "%u,%u,%u,%u",
                     mods_depressed, mods_latched, mods_locked, group);
    debug_emit("MODIFIERS", buf);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct app *app = data;
    char buf[64];
    (void)pointer;
    (void)serial;
    (void)surface;
    app->pointer_x = surface_x;
    checked_snprintf(buf, sizeof(buf), "%.1f:%.1f",
                     wl_fixed_to_double(surface_x),
                     wl_fixed_to_double(surface_y));
    debug_emit("POINTER_ENTER", buf);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)pointer;
    (void)serial;
    (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y)
{
    struct app *app = data;
    char buf[64];
    (void)pointer;
    (void)time;
    (void)surface_y;
    app->pointer_x = surface_x;
    checked_snprintf(buf, sizeof(buf), "%.1f",
                     wl_fixed_to_double(surface_x));
    debug_emit("POINTER_MOTION", buf);
}

static void move_cursor_to_surface_x(struct app *app, wl_fixed_t surface_x)
{
    double x = wl_fixed_to_double(surface_x);
    uint32_t chars = 0;
    if (x > TEXT_X)
        chars = (uint32_t)((x - TEXT_X + APPROX_CHAR_W / 2.0) / APPROX_CHAR_W);
    app->cursor = char_count_to_byte(app->text, app->text_len, chars);
    cursor_changed_by_user(app);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    struct app *app = data;
    (void)pointer;
    (void)serial;
    (void)time;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    require_true(button == 0x110 || button == 0x14a,
                 "unexpected pointer button: %u", button);

    move_cursor_to_surface_x(app, app->pointer_x);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t axis_source)
{
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    (void)touch;
    (void)time;
    const char *label = surface_label_for_touch(app, surface);
    require_true(label != NULL, "touch down for unexpected surface %p",
                 (void *)surface);

    double dx = wl_fixed_to_double(x);
    double dy = wl_fixed_to_double(y);
    struct touch_point *point = alloc_touch(app, id);
    point->active = 1;
    point->x = dx;
    point->y = dy;
    checked_copy(point->target, sizeof(point->target), label, "touch target");
    if (app->scene_kind == SCENE_NONE)
        emit_touch_event(app, "TOUCH_DOWN", id, dx, dy);
    else
        emit_scene_touch_event(app, "SURFACE_TOUCH_DOWN", label, id, dx, dy);
    request_redraw(app);

    if (!app->touch_debug)
        move_cursor_to_surface_x(app, x);

    if (app->scene_kind == SCENE_POPUP_SWITCH && surface == app->surface) {
        if (!app->scene_child_created) {
            create_switch_popup(app, 0, serial);
            app->scene_child_created = 1;
        } else if (!app->popup_switch_created) {
            create_switch_popup(app, 1, serial);
            app->popup_switch_created = 1;
        }
    }
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
                     uint32_t time, int32_t id)
{
    struct app *app = data;
    (void)touch;
    (void)serial;
    (void)time;
    struct touch_point *point = find_touch(app, id);
    require_true(point != NULL && point->active, "touch up for unknown id %d", id);
    point->active = 0;
    if (app->scene_kind == SCENE_NONE)
        emit_touch_event(app, "TOUCH_UP", id, point->x, point->y);
    else
        emit_scene_touch_event(app, "SURFACE_TOUCH_UP", point->target, id,
                               point->x, point->y);
    request_redraw(app);
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
                         int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    struct app *app = data;
    (void)touch;
    (void)time;
    double dx = wl_fixed_to_double(x);
    double dy = wl_fixed_to_double(y);
    struct touch_point *point = find_touch(app, id);
    require_true(point != NULL && point->active,
                 "touch motion for unknown id %d", id);
    point->x = dx;
    point->y = dy;
    if (app->scene_kind == SCENE_NONE)
        emit_touch_event(app, "TOUCH_MOTION", id, dx, dy);
    else
        emit_scene_touch_event(app, "SURFACE_TOUCH_MOTION", point->target, id,
                               dx, dy);
    request_redraw(app);
}

static void touch_frame(void *data, struct wl_touch *touch)
{
    struct app *app = data;
    (void)touch;
    debug_emit_u32("TOUCH_FRAME", (uint32_t)active_touch_count(app));
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
    struct app *app = data;
    (void)touch;
    for (int i = 0; i < MAX_TOUCHES; i++)
        app->touches[i].active = 0;
    debug_emit("TOUCH_CANCEL", NULL);
    request_redraw(app);
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    struct app *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        require_true(app->keyboard != NULL, "wl_seat_get_keyboard returned NULL");
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard) {
        fatal("seat removed keyboard capability");
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        require_true(app->pointer != NULL, "wl_seat_get_pointer returned NULL");
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && app->pointer) {
        fatal("seat removed pointer capability");
    }
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !app->touch) {
        app->touch = wl_seat_get_touch(seat);
        require_true(app->touch != NULL, "wl_seat_get_touch returned NULL");
        wl_touch_add_listener(app->touch, &touch_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH) && app->touch) {
        fatal("seat removed touch capability");
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        require_true(app->compositor == NULL, "duplicate wl_compositor global");
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           version >= 4 ? 4 : version);
        require_true(app->compositor != NULL, "bind wl_compositor failed");
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        require_true(app->shm == NULL, "duplicate wl_shm global");
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        require_true(app->shm != NULL, "bind wl_shm failed");
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        require_true(app->subcompositor == NULL,
                     "duplicate wl_subcompositor global");
        app->subcompositor = wl_registry_bind(
            registry, name, &wl_subcompositor_interface, 1);
        require_true(app->subcompositor != NULL,
                     "bind wl_subcompositor failed");
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        require_true(app->seat == NULL, "duplicate wl_seat global");
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     version >= 5 ? 5 : version);
        require_true(app->seat != NULL, "bind wl_seat failed");
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        require_true(app->data_device_manager == NULL,
                     "duplicate wl_data_device_manager global");
        app->data_device_manager = wl_registry_bind(
            registry, name, &wl_data_device_manager_interface,
            version >= 3 ? 3 : version);
        require_true(app->data_device_manager != NULL,
                     "bind wl_data_device_manager failed");
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        require_true(app->wm_base == NULL, "duplicate xdg_wm_base global");
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                        version >= 4 ? 4 : version);
        require_true(app->wm_base != NULL, "bind xdg_wm_base failed");
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        require_true(app->text_input_manager == NULL,
                     "duplicate zwp_text_input_manager_v3 global");
        app->text_input_manager = wl_registry_bind(
            registry, name, &zwp_text_input_manager_v3_interface, 1);
        require_true(app->text_input_manager != NULL,
                     "bind zwp_text_input_manager_v3 failed");
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        require_true(app->fractional_scale_manager == NULL,
                     "duplicate wp_fractional_scale_manager_v1 global");
        app->fractional_scale_manager = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
        require_true(app->fractional_scale_manager != NULL,
                     "bind wp_fractional_scale_manager_v1 failed");
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        app->output_global_count++;
        debug_emit_u32("OUTPUT_GLOBAL", app->output_global_count);
        if (app->output == NULL) {
            app->output = wl_registry_bind(
                registry, name, &wl_output_interface, version >= 4 ? 4 : version);
            require_true(app->output != NULL, "bind wl_output failed");
            wl_output_add_listener(app->output, &output_listener, app);
        }
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        require_true(app->decoration_manager == NULL,
                     "duplicate zxdg_decoration_manager_v1 global");
        app->decoration_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
        require_true(app->decoration_manager != NULL,
                     "bind zxdg_decoration_manager_v1 failed");
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- Setup/teardown ----------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    if (signal_app)
        signal_app->running = 0;
}

static void setup_wayland(struct app *app, const struct wayland_mode *mode)
{
    app->running = 1;
    app->held_clipboard_fd = -1;
    app->editable = mode->editable;
    app->provide_surrounding = mode->provide_surrounding;
    app->content_hint = mode->content_hint;
    app->content_purpose = mode->content_purpose;
    app->stale_trailing_newline_cursor = mode->stale_trailing_newline_cursor;
    app->echo_preedit_in_surrounding = mode->echo_preedit_in_surrounding;
    app->touch_debug = mode->touch_debug;
    app->fullscreen = mode->fullscreen;
    app->report_scale = mode->report_scale;
    app->render_pattern = mode->render_pattern;
    app->use_data_device = mode->use_data_device;
    app->paste_clipboard_on_selection = mode->clipboard_paste;
    app->clipboard_double_set = mode->clipboard_double_set;
    app->clipboard_generated_bytes = mode->clipboard_generated_bytes;
    app->clipboard_leave_fd_open = mode->clipboard_leave_fd_open;
    app->scene_kind = mode->scene_kind;
    app->scene_child_input_empty = mode->scene_child_input_empty;
    if (mode->clipboard_copy_text || mode->clipboard_generated_bytes > 0 ||
        mode->clipboard_leave_fd_open) {
        checked_copy(app->clipboard_copy_text,
                     sizeof(app->clipboard_copy_text),
                     mode->clipboard_copy_text ? mode->clipboard_copy_text : "x",
                     "clipboard copy text");
        app->copy_clipboard_on_focus = 1;
    }
    app->request_decoration = mode->request_decoration;
    app->win_w = WIN_W;
    app->win_h = WIN_H;
    app->display = wl_display_connect(NULL);
    if (!app->display)
        fatal("wl_display_connect failed");

    app->registry = wl_display_get_registry(app->display);
    require_true(app->registry != NULL, "wl_display_get_registry returned NULL");
    wl_registry_add_listener(app->registry, &registry_listener, app);
    if (wl_display_roundtrip(app->display) < 0)
        fatal("initial registry roundtrip failed: %s", strerror(errno));
    if (wl_display_roundtrip(app->display) < 0)
        fatal("seat/global roundtrip failed: %s", strerror(errno));
    debug_emit_u32("INITIAL_OUTPUT_GLOBALS", (uint32_t)app->output_global_count);

    require_true(app->compositor != NULL, "missing wl_compositor");
    require_true(app->shm != NULL, "missing wl_shm");
    if (mode->scene_kind == SCENE_SUBSURFACE)
        require_true(app->subcompositor != NULL, "missing wl_subcompositor");
    require_true(app->seat != NULL, "missing wl_seat");
    require_true(app->keyboard != NULL, "wl_seat missing keyboard capability");
    require_true(app->touch != NULL, "wl_seat missing touch capability");
    require_true(app->wm_base != NULL, "missing xdg_wm_base");
    if (mode->use_text_input)
        require_true(app->text_input_manager != NULL,
                     "missing zwp_text_input_manager_v3");
    if (mode->report_scale)
        require_true(app->fractional_scale_manager != NULL,
                     "missing wp_fractional_scale_manager_v1");
    if (mode->request_decoration)
        require_true(app->decoration_manager != NULL,
                     "missing zxdg_decoration_manager_v1");
    if (mode->use_data_device)
        require_true(app->data_device_manager != NULL,
                     "missing wl_data_device_manager");

    app->surface = wl_compositor_create_surface(app->compositor);
    require_true(app->surface != NULL,
                 "wl_compositor_create_surface returned NULL");
    app->xdg_surface = xdg_wm_base_get_xdg_surface(app->wm_base, app->surface);
    require_true(app->xdg_surface != NULL,
                 "xdg_wm_base_get_xdg_surface returned NULL");
    xdg_surface_add_listener(app->xdg_surface, &xdg_surface_listener, app);
    app->toplevel = xdg_surface_get_toplevel(app->xdg_surface);
    require_true(app->toplevel != NULL,
                 "xdg_surface_get_toplevel returned NULL");
    xdg_toplevel_add_listener(app->toplevel, &toplevel_listener, app);
    xdg_toplevel_set_title(app->toplevel, mode->title);
    xdg_toplevel_set_app_id(app->toplevel, mode->app_id);
    if (app->request_decoration) {
        app->decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(
                app->decoration_manager, app->toplevel);
        require_true(app->decoration != NULL,
                     "get_toplevel_decoration returned NULL");
        zxdg_toplevel_decoration_v1_add_listener(
            app->decoration, &decoration_listener, app);
        zxdg_toplevel_decoration_v1_set_mode(
            app->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (mode->fullscreen)
        xdg_toplevel_set_fullscreen(app->toplevel, NULL);
    else
        xdg_toplevel_set_min_size(app->toplevel, WIN_W, WIN_H);

    if (mode->use_text_input) {
        app->text_input =
            zwp_text_input_manager_v3_get_text_input(app->text_input_manager,
                                                     app->seat);
        require_true(app->text_input != NULL,
                     "zwp_text_input_manager_v3_get_text_input returned NULL");
        zwp_text_input_v3_add_listener(app->text_input, &text_input_listener,
                                       app);
    }
    if (mode->report_scale) {
        app->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                app->fractional_scale_manager, app->surface);
        require_true(app->fractional_scale != NULL,
                     "get_fractional_scale returned NULL");
        wp_fractional_scale_v1_add_listener(app->fractional_scale,
                                            &fractional_scale_listener, app);
    }

    if (mode->use_data_device) {
        app->data_device =
            wl_data_device_manager_get_data_device(app->data_device_manager,
                                                   app->seat);
        require_true(app->data_device != NULL,
                     "wl_data_device_manager_get_data_device returned NULL");
        wl_data_device_add_listener(app->data_device, &data_device_listener,
                                    app);
    }

    wl_surface_commit(app->surface);
    checked_flush(app->display);
}

static void teardown_wayland(struct app *app)
{
    retire_main_shm_pool(app->main_pool);
    app->main_pool = NULL;
    if (app->held_clipboard_fd >= 0) {
        close(app->held_clipboard_fd);
        app->held_clipboard_fd = -1;
    }
    if (app->text_input)
        zwp_text_input_v3_destroy(app->text_input);
    if (app->fractional_scale)
        wp_fractional_scale_v1_destroy(app->fractional_scale);
    if (app->clipboard_source)
        wl_data_source_destroy(app->clipboard_source);
    if (app->clipboard_offer)
        wl_data_offer_destroy(app->clipboard_offer);
    if (app->data_device)
        wl_data_device_destroy(app->data_device);
    if (app->pointer)
        wl_pointer_destroy(app->pointer);
    if (app->touch)
        wl_touch_destroy(app->touch);
    if (app->keyboard)
        wl_keyboard_destroy(app->keyboard);
    if (app->second_child_popup)
        xdg_popup_destroy(app->second_child_popup);
    if (app->second_child_xdg_surface)
        xdg_surface_destroy(app->second_child_xdg_surface);
    if (app->second_child_ready_callback)
        wl_callback_destroy(app->second_child_ready_callback);
    if (app->second_child_surface)
        wl_surface_destroy(app->second_child_surface);
    if (app->child_popup)
        xdg_popup_destroy(app->child_popup);
    if (app->child_xdg_surface)
        xdg_surface_destroy(app->child_xdg_surface);
    if (app->child_subsurface)
        wl_subsurface_destroy(app->child_subsurface);
    if (app->child_ready_callback)
        wl_callback_destroy(app->child_ready_callback);
    if (app->child_surface)
        wl_surface_destroy(app->child_surface);
    if (app->decoration)
        zxdg_toplevel_decoration_v1_destroy(app->decoration);
    if (app->toplevel)
        xdg_toplevel_destroy(app->toplevel);
    if (app->xdg_surface)
        xdg_surface_destroy(app->xdg_surface);
    if (app->surface)
        wl_surface_destroy(app->surface);
    if (app->text_input_manager)
        zwp_text_input_manager_v3_destroy(app->text_input_manager);
    if (app->fractional_scale_manager)
        wp_fractional_scale_manager_v1_destroy(app->fractional_scale_manager);
    if (app->decoration_manager)
        zxdg_decoration_manager_v1_destroy(app->decoration_manager);
    if (app->data_device_manager)
        wl_data_device_manager_destroy(app->data_device_manager);
    if (app->wm_base)
        xdg_wm_base_destroy(app->wm_base);
    if (app->seat)
        wl_seat_destroy(app->seat);
    if (app->shm)
        wl_shm_destroy(app->shm);
    if (app->subcompositor)
        wl_subcompositor_destroy(app->subcompositor);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->output)
        wl_output_destroy(app->output);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
}

/* --- Commands ----------------------------------------------------------- */

static uint32_t text_input_content_purpose_from_env(void)
{
    const char *value = getenv("TAWC_DEBUG_CONTENT_PURPOSE");
    if (!value || !value[0] || strcmp(value, "normal") == 0)
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
    if (strcmp(value, "url") == 0)
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL;
    if (strcmp(value, "password") == 0)
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    fatal("unknown TAWC_DEBUG_CONTENT_PURPOSE=%s", value);
    return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
}

static int cmd_text_input(int argc, char **argv)
{
    struct wayland_mode mode = {
        .title = "tawc wayland text-input debug",
        .app_id = "wayland-debug-app",
        .use_text_input = 1,
        .editable = 1,
        .provide_surrounding = 1,
        .content_purpose = text_input_content_purpose_from_env(),
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_text_input_no_surrounding(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland text-input no-surrounding debug",
        .app_id = "wayland-debug-app-no-surrounding",
        .use_text_input = 1,
        .editable = 0,
        .provide_surrounding = 0,
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_text_input_stale_newline(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland text-input stale-newline debug",
        .app_id = "wayland-debug-app-stale-newline",
        .use_text_input = 1,
        .editable = 1,
        .provide_surrounding = 1,
        .stale_trailing_newline_cursor = 1,
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_text_input_echo_preedit(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland text-input echo-preedit debug",
        .app_id = "wayland-debug-app-echo-preedit",
        .use_text_input = 1,
        .editable = 1,
        .provide_surrounding = 1,
        .echo_preedit_in_surrounding = 1,
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int run_scene_command(const struct wayland_mode *mode);

static int run_clipboard_copy(int argc, char **argv, int double_set)
{
    char text[MAX_TEXT];
    struct wayland_mode mode = {
        .title = "tawc wayland clipboard copy debug",
        .app_id = "wayland-debug-app-clipboard-copy",
        .use_data_device = 1,
        .clipboard_double_set = double_set,
        .editable = 0,
        .provide_surrounding = 0,
    };

    if (argc < 2)
        fatal("clipboard-copy requires text argument");
    text[0] = '\0';
    for (int i = 1; i < argc; i++) {
        size_t used = strlen(text);
        size_t arg_len = strlen(argv[i]);
        size_t sep = used > 0 ? 1 : 0;
        if (used + sep + arg_len >= sizeof(text))
            fatal("clipboard-copy text too long");
        if (sep)
            text[used++] = ' ';
        memcpy(text + used, argv[i], arg_len + 1);
    }
    mode.clipboard_copy_text = text;
    return run_scene_command(&mode);
}

static int cmd_clipboard_copy(int argc, char **argv)
{
    return run_clipboard_copy(argc, argv, 0);
}

static int cmd_clipboard_copy_double(int argc, char **argv)
{
    return run_clipboard_copy(argc, argv, 1);
}

static int cmd_clipboard_copy_overcap(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland clipboard overcap debug",
        .app_id = "wayland-debug-app-clipboard-overcap",
        .use_data_device = 1,
        .clipboard_generated_bytes = CLIPBOARD_OVERCAP_BYTES,
        .editable = 0,
        .provide_surrounding = 0,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_clipboard_copy_timeout(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland clipboard timeout debug",
        .app_id = "wayland-debug-app-clipboard-timeout",
        .use_data_device = 1,
        .clipboard_copy_text = "clipboard source should time out",
        .clipboard_leave_fd_open = 1,
        .editable = 0,
        .provide_surrounding = 0,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_clipboard_paste(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland clipboard paste debug",
        .app_id = "wayland-debug-app-clipboard-paste",
        .use_data_device = 1,
        .clipboard_paste = 1,
        .editable = 0,
        .provide_surrounding = 0,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_touch(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland touch debug",
        .app_id = "wayland-debug-app-touch",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .touch_debug = 1,
        .fullscreen = 1,
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_scale(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland scale debug",
        .app_id = "wayland-debug-app-scale",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .report_scale = 1,
    };

    (void)argc;
    (void)argv;

    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    setup_wayland(&app, &mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_render_pattern(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland render pattern",
        .app_id = "wayland-debug-app-render-pattern",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .fullscreen = 1,
        .render_pattern = 1,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_initial_configure(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland initial configure debug",
        .app_id = "wayland-debug-app-initial-configure",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .request_decoration = 1,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int run_scene_command(const struct wayland_mode *mode)
{
    struct app app;
    memset(&app, 0, sizeof(app));
    signal_app = &app;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    setup_wayland(&app, mode);

    while (app.running) {
        if (wl_display_dispatch(app.display) < 0) {
            if (errno == EINTR && !app.running)
                break;
            fatal("wl_display_dispatch failed: %s", strerror(errno));
        }
    }

    teardown_wayland(&app);
    return 0;
}

static int cmd_subsurface(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland subsurface debug",
        .app_id = "wayland-debug-app-subsurface",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .fullscreen = 1,
        .scene_kind = SCENE_SUBSURFACE,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_subsurface_input_empty(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland subsurface input-empty debug",
        .app_id = "wayland-debug-app-subsurface-input-empty",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .fullscreen = 1,
        .scene_kind = SCENE_SUBSURFACE,
        .scene_child_input_empty = 1,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_popup(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland popup debug",
        .app_id = "wayland-debug-app-popup",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .fullscreen = 1,
        .scene_kind = SCENE_POPUP,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

static int cmd_popup_switch(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland popup switch debug",
        .app_id = "wayland-debug-app-popup-switch",
        .use_text_input = 0,
        .editable = 0,
        .provide_surrounding = 0,
        .fullscreen = 1,
        .scene_kind = SCENE_POPUP_SWITCH,
    };

    (void)argc;
    (void)argv;
    return run_scene_command(&mode);
}

typedef int (*command_fn)(int argc, char **argv);

struct command {
    const char *name;
    const char *description;
    command_fn fn;
};

static const struct command commands[] = {
    { "text-input", "Minimal editable text-input-v3 surface", cmd_text_input },
    { "text-input-no-surrounding",
      "Text-input surface that never sends surrounding",
      cmd_text_input_no_surrounding },
    { "text-input-echo-preedit",
      "Text-input surface that includes preedit in surrounding reports",
      cmd_text_input_echo_preedit },
    { "text-input-stale-newline",
      "Text-input surface that reports cursor before trailing newlines",
      cmd_text_input_stale_newline },
    { "clipboard-copy", "Set wl_data_device clipboard text",
      cmd_clipboard_copy },
    { "clipboard-copy-double",
      "Set wl_data_device clipboard text twice per copy (GTK3 style)",
      cmd_clipboard_copy_double },
    { "clipboard-copy-overcap", "Set oversized wl_data_device clipboard text",
      cmd_clipboard_copy_overcap },
    { "clipboard-copy-timeout", "Set non-closing wl_data_device clipboard text",
      cmd_clipboard_copy_timeout },
    { "clipboard-paste", "Read focused wl_data_device clipboard text",
      cmd_clipboard_paste },
    { "touch", "Fullscreen touch visualizer", cmd_touch },
    { "render-pattern", "Fullscreen deterministic SHM color pattern",
      cmd_render_pattern },
    { "scale", "Report fractional scale changes", cmd_scale },
    { "initial-configure", "Report initial output/configure sequencing",
      cmd_initial_configure },
    { "subsurface", "Fullscreen toplevel with a touchable subsurface",
      cmd_subsurface },
    { "subsurface-input-empty",
      "Fullscreen toplevel with a non-input subsurface", cmd_subsurface_input_empty },
    { "popup", "Fullscreen toplevel with a touchable xdg_popup", cmd_popup },
    { "popup-switch", "Two touch-opened grabbed xdg_popups",
      cmd_popup_switch },
    { NULL, NULL, NULL },
};

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command>\n\nCommands:\n", prog);
    for (const struct command *cmd = commands; cmd->name; cmd++)
        fprintf(stderr, "  %-16s %s\n", cmd->name, cmd->description);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    for (const struct command *cmd = commands; cmd->name; cmd++) {
        if (strcmp(argv[1], cmd->name) == 0)
            return cmd->fn(argc - 1, argv + 1);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
