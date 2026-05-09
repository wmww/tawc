/*
 * gtk4-debug-app: A GTK4 debug/test app for the tawc compositor.
 *
 * Sister binary to gtk3-debug-app, ported to GTK4. Same subcommand CLI and
 * output protocol (TAWC_DEBUG: prefix parsed by the Rust harness), so tests
 * can target GTK4 (always hardware-buffered via android_wlegl) and GTK3
 * (SHM vs AHB selectable via GDK_GL) through one harness.
 *
 * Usage: gtk4-debug-app <command>
 *
 * Commands:
 *   text-input                  Open a text view for testing text input
 *   text-input-no-surrounding   Bare IM context without set_surrounding (terminal-shaped)
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Output protocol ---------------------------------------------------- */

static void debug_emit(const char *tag, const char *value)
{
    if (!value || !value[0]) {
        printf("TAWC_DEBUG:%s\n", tag);
        fflush(stdout);
        return;
    }
    /* Escape so multi-line values stay on a single TAWC_DEBUG line — the
     * harness parses one value per stdout line. \n -> \\n, \r -> \\r,
     * \\ -> \\\\. The harness's stripping is plain-text so no further
     * decoding is needed for ordinary asserts. */
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

/* --- text-input command ------------------------------------------------- */

static GMainLoop *main_loop;
static GtkWidget *text_view;

static void on_window_destroy(GtkWidget *window, gpointer user_data)
{
    (void)window;
    (void)user_data;
    if (main_loop)
        g_main_loop_quit(main_loop);
}

static void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    (void)user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    debug_emit("TEXT_CHANGED", text);
    g_free(text);
}

static void on_preedit_changed(GtkTextView *tv, const char *preedit, gpointer user_data)
{
    (void)tv;
    (void)user_data;
    debug_emit("PREEDIT", preedit);
}

static void on_mark_set(GtkTextBuffer *buffer, GtkTextIter *location,
                        GtkTextMark *mark, gpointer user_data)
{
    (void)user_data;
    if (mark != gtk_text_buffer_get_insert(buffer))
        return;
    int offset = gtk_text_iter_get_offset(location);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", offset);
    debug_emit("CURSOR_POS", buf);
}

static gboolean emit_ready(gpointer user_data)
{
    GtkWidget *window = GTK_WIDGET(user_data);

    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    debug_emit("RENDERER", renderer ? G_OBJECT_TYPE_NAME(renderer) : "unknown");

    FILE *maps = fopen("/proc/self/maps", "r");
    gboolean vulkan = FALSE;
    if (maps) {
        char buf[512];
        while (fgets(buf, sizeof(buf), maps)) {
            if (strstr(buf, "libvulkan")) {
                vulkan = TRUE;
                break;
            }
        }
        fclose(maps);
    }
    debug_emit("VULKAN_LOADED", vulkan ? "yes" : "no");

    debug_emit("READY", NULL);
    return G_SOURCE_REMOVE;
}

static void on_map(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    g_idle_add(emit_ready, widget);
}

/* Big Monospace text — readable on a phone screen at arm's length. GTK4
 * dropped gtk_widget_override_font, so we install a CSS rule on the default
 * display. The size is also documented in tests/input.rs's tap-coord block,
 * since changing it affects how `input tap` maps to character offsets. */
static void apply_big_monospace_css(void)
{
    static const char *css =
        "textview, textview text { font-family: Monospace; font-size: 28pt; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

static int cmd_text_input(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    gtk_init();
    apply_big_monospace_css();

    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "tawc debug: text-input (gtk4)");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_margin_top(scroll, 80);
    gtk_widget_set_margin_bottom(scroll, 40);
    gtk_widget_set_margin_start(scroll, 40);
    gtk_widget_set_margin_end(scroll, 40);
    gtk_window_set_child(GTK_WINDOW(window), scroll);

    text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text_view);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), NULL);
    g_signal_connect(text_view, "preedit-changed", G_CALLBACK(on_preedit_changed), NULL);
    g_signal_connect(buffer, "mark-set", G_CALLBACK(on_mark_set), NULL);
    g_signal_connect(window, "map", G_CALLBACK(on_map), NULL);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(text_view);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
    g_main_loop_unref(main_loop);
    main_loop = NULL;
    return 0;
}

/* --- text-input-no-surrounding command ---------------------------------- */
/*
 * The text-input mode above puts a real GtkTextView on screen — its
 * IM context unconditionally pushes set_surrounding_text on every cursor
 * move and edit, so we exercise the "rich editor" shape of text-input-v3
 * client. This mode covers the OTHER shape: a client that enables
 * text-input-v3 (so the soft keyboard appears) but never calls
 * gtk_im_context_set_surrounding, because it has no editable buffer
 * behind whatever the user is looking at. VTE under Wayland is the
 * motivating real-world example — terminals don't model an edit buffer
 * behind the prompt.
 *
 * Implementation: a plain drawing area as the wl_surface, a manually
 * managed GtkIMMulticontext attached and focus-in'd on map, signals
 * piped to TAWC_DEBUG: lines. Crucially we never call
 * gtk_im_context_set_surrounding — the bug under test is the compositor
 * sending `delete_surrounding_text` to clients that haven't pushed
 * surrounding (the Wayland protocol leaves the cursor index undefined
 * in that case, and VTE responds by closing the connection). The
 * compositor must instead synthesise a wl_keyboard Backspace.
 *
 * Asserts to make on top of this app:
 *   - TAWC_DEBUG:DELETE_SURROUNDING:* must NEVER appear.
 *   - On `am broadcast DELETE_SURROUNDING_TEXT before=N`, the harness
 *     should see N TAWC_DEBUG:KEY:BackSpace events instead.
 */

static GtkIMContext *bare_im_context;

static void on_im_commit(GtkIMContext *im, const char *str, gpointer user_data)
{
    (void)im;
    (void)user_data;
    debug_emit("COMMIT", str);
}

static void on_im_preedit_changed(GtkIMContext *im, gpointer user_data)
{
    (void)user_data;
    char *preedit = NULL;
    gtk_im_context_get_preedit_string(im, &preedit, NULL, NULL);
    debug_emit("PREEDIT", preedit ? preedit : "");
    g_free(preedit);
}

/* delete-surrounding fires when the IM module asks GTK to delete bytes
 * around the cursor. Under our compositor's fix this signal must never
 * reach a surrounding-less client — the test scenario watches for it
 * to assert absence. We log and return TRUE so the IM module thinks we
 * handled it (refusing keeps the app alive either way; this avoids GTK
 * fallback behaviour confusing the trace). */
static gboolean on_im_delete_surrounding(GtkIMContext *im, gint offset, gint n_chars,
                                         gpointer user_data)
{
    (void)im;
    (void)user_data;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d:%d", offset, n_chars);
    debug_emit("DELETE_SURROUNDING", buf);
    return TRUE;
}

/* Refuse the retrieve-surrounding callback so the IM module knows we
 * have no surrounding text. Returning FALSE is the documented "I don't
 * support surrounding" answer; without this the IM module would assume
 * an empty surrounding and start pushing set_surrounding("",...) under
 * us, which defeats the purpose of this mode. */
static gboolean on_im_retrieve_surrounding(GtkIMContext *im, gpointer user_data)
{
    (void)im;
    (void)user_data;
    return FALSE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    (void)user_data;
    const char *name = gdk_keyval_name(keyval);
    debug_emit("KEY", name ? name : "");
    return GDK_EVENT_PROPAGATE;
}

static void on_bare_map(GtkWidget *window, gpointer user_data)
{
    GtkWidget *area = GTK_WIDGET(user_data);
    /* Activate the IM context once the surface exists. Wayland's
     * text-input-v3 enable+commit only goes out after the surface has
     * keyboard focus, which itself depends on the surface being mapped. */
    gtk_im_context_set_client_widget(bare_im_context, area);
    gtk_im_context_focus_in(bare_im_context);

    g_idle_add(emit_ready, window);
}

static int cmd_text_input_no_surrounding(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    gtk_init();

    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window),
                         "tawc debug: text-input-no-surrounding (gtk4)");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 400);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    /* Plain drawing area as the wl_surface — no editor widget, so GTK
     * never installs a built-in IM context that would push surrounding. */
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_focusable(area, TRUE);
    gtk_window_set_child(GTK_WINDOW(window), area);

    bare_im_context = gtk_im_multicontext_new();
    g_signal_connect(bare_im_context, "commit",
                     G_CALLBACK(on_im_commit), NULL);
    g_signal_connect(bare_im_context, "preedit-changed",
                     G_CALLBACK(on_im_preedit_changed), NULL);
    g_signal_connect(bare_im_context, "delete-surrounding",
                     G_CALLBACK(on_im_delete_surrounding), NULL);
    g_signal_connect(bare_im_context, "retrieve-surrounding",
                     G_CALLBACK(on_im_retrieve_surrounding), NULL);

    /* Capture wl_keyboard key events directly so the test can assert
     * deletion arrives as Backspace presses. The IM controller would
     * normally swallow keys it considers part of input — set CAPTURE
     * phase so we observe everything before any IM filtering. */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(area, key_ctrl);

    g_signal_connect(window, "map", G_CALLBACK(on_bare_map), area);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(area);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
    gtk_im_context_focus_out(bare_im_context);
    g_clear_object(&bare_im_context);
    g_main_loop_unref(main_loop);
    main_loop = NULL;
    return 0;
}

/* --- Command dispatch --------------------------------------------------- */

typedef int (*command_fn)(int argc, char *argv[]);

struct command {
    const char *name;
    const char *description;
    command_fn fn;
};

static const struct command commands[] = {
    { "text-input", "Open a text view for testing text input", cmd_text_input },
    { "text-input-no-surrounding",
      "Bare IM context without set_surrounding (terminal-shaped)",
      cmd_text_input_no_surrounding },
    { NULL, NULL, NULL },
};

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command>\n\nCommands:\n", prog);
    for (const struct command *cmd = commands; cmd->name; cmd++)
        fprintf(stderr, "  %-16s %s\n", cmd->name, cmd->description);
}

int main(int argc, char *argv[])
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
