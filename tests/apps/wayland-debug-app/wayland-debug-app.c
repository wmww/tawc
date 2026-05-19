/*
 * wayland-debug-app: toolkitless Wayland client for tawc integration tests.
 *
 * Subcommand CLI; output is the TAWC_DEBUG: protocol parsed by the Rust
 * integration harness.
 *
 * This is test code, not production UI. Missing globals, failed Wayland
 * requests, invalid protocol ordering, truncated protocol strings, and
 * internal state invariants abort the process so the harness sees a loud
 * failure instead of a tolerant client hiding a compositor bug.
 *
 * Usage: wayland-debug-app <command>
 *
 * Commands:
 *   text-input                  Minimal editable text-input-v3 surface
 *   text-input-no-surrounding   Text-input surface that never sends surrounding
 *   touch                       Fullscreen touch visualizer
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <wayland-client.h>

#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define WIN_W 640
#define WIN_H 240
#define TEXT_X 24.0
#define TEXT_Y 96.0
#define FONT_SIZE 32.0
#define APPROX_CHAR_W 19.0
#define MAX_TEXT 8192
#define MAX_TOUCHES 16

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

/* --- App state ---------------------------------------------------------- */

struct shm_buffer {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
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
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_v3 *text_input;

    int running;
    int configured;
    int ready_emitted;
    int text_input_enabled;
    int editable;
    int provide_surrounding;
    int touch_debug;
    int fullscreen;
    int dynamic_size;
    int win_w;
    int win_h;
    wl_fixed_t pointer_x;

    char text[MAX_TEXT];
    size_t text_len;
    size_t cursor;
    char preedit[MAX_TEXT];
    struct text_input_pending pending_text_input;
    struct touch_point touches[MAX_TOUCHES];
};

struct wayland_mode {
    const char *title;
    const char *app_id;
    int use_text_input;
    int editable;
    int provide_surrounding;
    int touch_debug;
    int fullscreen;
    int dynamic_size;
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

    zwp_text_input_v3_set_text_change_cause(app->text_input, cause);
    zwp_text_input_v3_set_surrounding_text(
        app->text_input, app->text, (int32_t)app->cursor, (int32_t)app->cursor);
    zwp_text_input_v3_commit(app->text_input);
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

    if (changed)
        changed_by_input_method(app);
    else if (preedit_changed)
        request_redraw(app);
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
    wl_buffer_destroy(buf->buffer);
    munmap(buf->data, buf->size);
    free(buf);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

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

static void request_redraw(struct app *app)
{
    if (!app->configured || !app->surface || !app->shm)
        return;

    int width = app->win_w > 0 ? app->win_w : WIN_W;
    int height = app->win_h > 0 ? app->win_h : WIN_H;
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = create_shm_fd(size);
    if (fd < 0)
        fatal("create shm failed: %s", strerror(errno));

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        int saved_errno = errno;
        perror("mmap");
        close(fd);
        fatal("mmap failed: %s", strerror(saved_errno));
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    struct shm_buffer *buf = calloc(1, sizeof(*buf));
    require_true(pool != NULL, "wl_shm_create_pool returned NULL");
    require_true(buf != NULL, "calloc shm_buffer failed");
    buf->data = data;
    buf->size = size;
    buf->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_ARGB8888);
    require_true(buf->buffer != NULL, "wl_shm_pool_create_buffer returned NULL");
    wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
    wl_shm_pool_destroy(pool);
    close(fd);

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t *cr = cairo_create(cs);
    require_true(cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS,
                 "cairo surface failed: %s",
                 cairo_status_to_string(cairo_surface_status(cs)));
    require_true(cairo_status(cr) == CAIRO_STATUS_SUCCESS,
                 "cairo context failed: %s",
                 cairo_status_to_string(cairo_status(cr)));

    if (app->touch_debug) {
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

    wl_surface_attach(app->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, width, height);
    wl_surface_commit(app->surface);
    checked_flush(app->display);

    if (!app->ready_emitted) {
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

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = 1;
    request_redraw(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    struct app *app = data;
    (void)toplevel;
    (void)states;
    if (app->dynamic_size && width > 0 && height > 0) {
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
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
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
        app->text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
        ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_set_cursor_rectangle(app->text_input, (int32_t)TEXT_X,
                                           (int32_t)(TEXT_Y - FONT_SIZE),
                                           2, (int32_t)(FONT_SIZE + 8.0));
    if (app->provide_surrounding)
        zwp_text_input_v3_set_surrounding_text(app->text_input, app->text,
                                               (int32_t)app->cursor,
                                               (int32_t)app->cursor);
    zwp_text_input_v3_commit(app->text_input);
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
    zwp_text_input_v3_disable(text_input);
    zwp_text_input_v3_commit(text_input);
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
    (void)serial;
    require_true(app->text_input_enabled, "done before text-input enter");
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
    (void)keyboard;
    (void)serial;
    (void)keys;
    require_true(surface == app->surface,
                 "keyboard enter for unexpected surface %p", (void *)surface);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    struct app *app = data;
    (void)keyboard;
    (void)serial;
    (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    if (key == KEY_BACKSPACE) {
        debug_emit("KEY", "BackSpace");
        if (!app->editable)
            return;
        if (app->cursor > 0) {
            size_t start = prev_char_start(app->text, app->cursor);
            delete_range(app, start, app->cursor);
            changed_by_input_method(app);
        }
    } else if (key == KEY_DELETE) {
        debug_emit("KEY", "Delete");
        if (!app->editable)
            return;
        if (app->cursor < app->text_len) {
            size_t end = next_char_end(app->text, app->text_len, app->cursor);
            delete_range(app, app->cursor, end);
            changed_by_input_method(app);
        }
    } else if (key == KEY_ENTER) {
        debug_emit("KEY", "Return");
        if (!app->editable)
            return;
        insert_text(app, "\n");
        changed_by_input_method(app);
    } else if (key == KEY_TAB) {
        debug_emit("KEY", "Tab");
        if (!app->editable)
            return;
        insert_text(app, "\t");
        changed_by_input_method(app);
    } else {
        char buf[32];
        checked_snprintf(buf, sizeof(buf), "%u", key);
        debug_emit("KEY", buf);
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
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
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
    (void)pointer;
    (void)serial;
    (void)surface_y;
    require_true(surface == app->surface,
                 "pointer enter for unexpected surface %p", (void *)surface);
    app->pointer_x = surface_x;
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
    (void)pointer;
    (void)time;
    (void)surface_y;
    app->pointer_x = surface_x;
}

static void move_cursor_to_surface_x(struct app *app, wl_fixed_t surface_x)
{
    double x = wl_fixed_to_double(surface_x);
    uint32_t chars = 0;
    if (x > TEXT_X)
        chars = (uint32_t)((x - TEXT_X + APPROX_CHAR_W / 2.0) / APPROX_CHAR_W);
    app->cursor = char_count_to_byte(app->text, app->text_len, chars);
    if (app->preedit[0]) {
        insert_text(app, app->preedit);
        app->preedit[0] = '\0';
        debug_emit("PREEDIT", "");
        emit_text_and_cursor(app);
    }
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
    (void)serial;
    (void)time;
    require_true(surface == app->surface,
                 "touch down for unexpected surface %p", (void *)surface);

    double dx = wl_fixed_to_double(x);
    double dy = wl_fixed_to_double(y);
    struct touch_point *point = alloc_touch(app, id);
    point->active = 1;
    point->x = dx;
    point->y = dy;
    emit_touch_event(app, "TOUCH_DOWN", id, dx, dy);
    request_redraw(app);

    if (!app->touch_debug)
        move_cursor_to_surface_x(app, x);
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
    emit_touch_event(app, "TOUCH_UP", id, point->x, point->y);
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
    emit_touch_event(app, "TOUCH_MOTION", id, dx, dy);
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
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        require_true(app->seat == NULL, "duplicate wl_seat global");
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                     version >= 5 ? 5 : version);
        require_true(app->seat != NULL, "bind wl_seat failed");
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        require_true(app->wm_base == NULL, "duplicate xdg_wm_base global");
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                        1);
        require_true(app->wm_base != NULL, "bind xdg_wm_base failed");
        xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        require_true(app->text_input_manager == NULL,
                     "duplicate zwp_text_input_manager_v3 global");
        app->text_input_manager = wl_registry_bind(
            registry, name, &zwp_text_input_manager_v3_interface, 1);
        require_true(app->text_input_manager != NULL,
                     "bind zwp_text_input_manager_v3 failed");
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
    app->editable = mode->editable;
    app->provide_surrounding = mode->provide_surrounding;
    app->touch_debug = mode->touch_debug;
    app->fullscreen = mode->fullscreen;
    app->dynamic_size = mode->dynamic_size;
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

    require_true(app->compositor != NULL, "missing wl_compositor");
    require_true(app->shm != NULL, "missing wl_shm");
    require_true(app->seat != NULL, "missing wl_seat");
    require_true(app->keyboard != NULL, "wl_seat missing keyboard capability");
    require_true(app->pointer != NULL, "wl_seat missing pointer capability");
    require_true(app->touch != NULL, "wl_seat missing touch capability");
    require_true(app->wm_base != NULL, "missing xdg_wm_base");
    if (mode->use_text_input)
        require_true(app->text_input_manager != NULL,
                     "missing zwp_text_input_manager_v3");

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

    wl_surface_commit(app->surface);
    checked_flush(app->display);
}

static void teardown_wayland(struct app *app)
{
    if (app->text_input)
        zwp_text_input_v3_destroy(app->text_input);
    if (app->pointer)
        wl_pointer_destroy(app->pointer);
    if (app->touch)
        wl_touch_destroy(app->touch);
    if (app->keyboard)
        wl_keyboard_destroy(app->keyboard);
    if (app->toplevel)
        xdg_toplevel_destroy(app->toplevel);
    if (app->xdg_surface)
        xdg_surface_destroy(app->xdg_surface);
    if (app->surface)
        wl_surface_destroy(app->surface);
    if (app->text_input_manager)
        zwp_text_input_manager_v3_destroy(app->text_input_manager);
    if (app->wm_base)
        xdg_wm_base_destroy(app->wm_base);
    if (app->seat)
        wl_seat_destroy(app->seat);
    if (app->shm)
        wl_shm_destroy(app->shm);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
}

/* --- Commands ----------------------------------------------------------- */

static int cmd_text_input(int argc, char **argv)
{
    static const struct wayland_mode mode = {
        .title = "tawc wayland text-input debug",
        .app_id = "wayland-debug-app",
        .use_text_input = 1,
        .editable = 1,
        .provide_surrounding = 1,
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
        .dynamic_size = 1,
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
    { "touch", "Fullscreen touch visualizer", cmd_touch },
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
