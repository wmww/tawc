// ando — run an Android command from inside the rootfs.
//
// Named like sudo, but for Android: `ando [flags] <cmd> [args…]` asks
// the tawc app process (which never had tawcroot's seccomp filter) to
// spawn <cmd> as a plain Android process, wired to this client's real
// stdin/stdout/stderr (passed via SCM_RIGHTS — tty semantics survive)
// and started in the caller's cwd (passed as an O_PATH fd; tawcroot
// translates the open, fds aren't virtualized). The sudo-style flags
// (-E, -D, -s, -u, -r) are all client-side: env lines, a chdir before
// the cwd open, and argv rewrites. Protocol and design: notes/ando.md;
// broker: compositor/src/ando.rs.
//
// Wire order: fd message first (1 byte + SCM_RIGHTS[stdin, stdout,
// stderr, cwd]), then the text header, then "SIG <n>" lines out /
// one "EXIT <code>" line back.
//
// Static bionic build (tawcroot/ando/build.sh), installed into every
// rootfs at /usr/local/bin/ando by AndoInstallProvider.

#define _GNU_SOURCE  // O_PATH

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern char **environ;

// Path override, mainly for tests; normal use never needs it.
#define SOCKET_ENV "TAWC_ANDO_SOCKET"
// The broker's socket as seen from inside every rootfs: the share-dir
// bind (like the wayland/kumquat sockets). tawcroot/proot translate
// the connect path through the bind; chroot resolves it natively. Keep
// the basename in sync with AppPaths.andoSocket (Kotlin).
#define SOCKET_DEFAULT "/usr/share/tawc/ando.sock"

// argv[0] override for the -u/-r su rewrite (test hook, like
// SOCKET_ENV): lets unrooted tests assert the constructed argv.
#define SU_ENV "TAWC_ANDO_SU"

// Broker header line limit (compositor/src/ando.rs MAX_LINE, counted
// including the newline). -E vars over it are skipped with a warning
// instead of killing the connection mid-header.
#define MAX_LINE 65536

// Exit codes for ando's own failures; the child's code is passed
// through verbatim (128+sig for signal deaths, mirroring shells).
#define EXIT_NO_BROKER 127
#define EXIT_PROTOCOL 125

static int sock_fd = -1;

static void usage(void) {
    fprintf(stderr,
            "usage: ando [-E | --preserve-env[=LIST]] [-D dir] [-s] [-u user | -r]\n"
            "            [-e K=V]... [--] [cmd [args...]]\n"
            "Run cmd as a plain Android process (no rootfs view, no fake root).\n"
            "  -e, --env K=V       set an extra environment variable for cmd\n"
            "  -E, --preserve-env  forward the guest env (minus PATH, LD_PRELOAD,\n"
            "                      LD_LIBRARY_PATH); =LIST forwards only those vars\n"
            "  -D, --chdir dir     start cmd in dir instead of the caller's cwd\n"
            "  -s, --shell         run /system/bin/sh (cmd args become sh -c '...')\n"
            "  -u, --user user     run cmd as user via Android su (needs root)\n"
            "  -r                  alias for --user=root\n"
            "The Android-side env is the app process's own plus $TERM and the above.\n");
}

// Async-signal-safe "SIG <n>\n" writer. Forwarded signals reach the
// whole Android-side process group via the broker's kill(-pgid, n).
static void forward_signal(int sig) {
    char buf[16];
    size_t i = 0;
    buf[i++] = 'S';
    buf[i++] = 'I';
    buf[i++] = 'G';
    buf[i++] = ' ';
    if (sig >= 10) buf[i++] = (char)('0' + sig / 10);
    buf[i++] = (char)('0' + sig % 10);
    buf[i++] = '\n';
    if (sock_fd >= 0) {
        ssize_t r = write(sock_fd, buf, i);
        (void)r;
    }
}

// notes/exec-broker.md "Value encoding": \ -> \\, LF -> \n, CR -> \r.
// Returns a malloc'd string.
static char *encode_value(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    char *p = out;
    for (const char *c = s; *c; c++) {
        switch (*c) {
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        default: *p++ = *c;
        }
    }
    *p = '\0';
    return out;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

// One header line: raw prefix (caller-controlled, e.g. "ARGV " or
// "ENV TERM="), encoded value, newline.
static int send_line(int fd, const char *prefix, const char *value) {
    char *enc = encode_value(value);
    if (!enc) return -1;
    int rc = write_all(fd, prefix, strlen(prefix));
    if (rc == 0) rc = write_all(fd, enc, strlen(enc));
    if (rc == 0) rc = write_all(fd, "\n", 1);
    free(enc);
    return rc;
}

// "ENV K=V": only the value half is encoded — callers reject or skip
// keys carrying LF/CR (same rule as the exec broker).
static int send_env_kv(int fd, const char *key, size_t keylen, const char *value) {
    if (write_all(fd, "ENV ", 4) != 0) return -1;
    if (write_all(fd, key, keylen) != 0) return -1;
    if (write_all(fd, "=", 1) != 0) return -1;
    char *enc = encode_value(value);
    if (!enc) return -1;
    int rc = write_all(fd, enc, strlen(enc));
    if (rc == 0) rc = write_all(fd, "\n", 1);
    free(enc);
    return rc;
}

// Guest values of these are rootfs paths that are meaningless or
// breaking Android-side (no /system/bin in guest PATH; LD_* is
// libhybris baggage). An explicit -e or --preserve-env=NAME still
// forwards them — explicit wins over policy.
static int env_blocked(const char *key, size_t keylen) {
    static const char *const block[] = { "PATH", "LD_PRELOAD", "LD_LIBRARY_PATH" };
    for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++) {
        if (strlen(block[i]) == keylen && memcmp(key, block[i], keylen) == 0) return 1;
    }
    return 0;
}

// Forward one guest var, skipping (with a warning) anything the wire
// can't carry — an over-MAX_LINE value or a key with LF/CR (only the
// value half is encoded) — instead of dying mid-header.
static int forward_env_var(int fd, const char *key, size_t keylen, const char *value) {
    if (memchr(key, '\n', keylen) || memchr(key, '\r', keylen)) {
        fprintf(stderr, "ando: skipping env var with unencodable name\n");
        return 0;
    }
    size_t line = 4 + keylen + 1 + 1;  // "ENV " K "=" … "\n"
    for (const char *c = value; *c; c++) {
        line += (*c == '\\' || *c == '\n' || *c == '\r') ? 2 : 1;
    }
    if (line > MAX_LINE) {
        fprintf(stderr, "ando: skipping oversized env var %.*s\n", (int)keylen, key);
        return 0;
    }
    return send_env_kv(fd, key, keylen, value);
}

// sudo-style join for sh -c: every byte outside [A-Za-z0-9_./=:,+@%^-]
// is backslash-escaped, args joined with single spaces. Returns a
// malloc'd string.
static char *shell_join(char **args, int n) {
    size_t cap = 1;
    for (int i = 0; i < n; i++) cap += strlen(args[i]) * 2 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    char *p = out;
    for (int i = 0; i < n; i++) {
        if (i > 0) *p++ = ' ';
        for (const char *c = args[i]; *c; c++) {
            unsigned char b = (unsigned char)*c;
            if (!((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                  (b >= '0' && b <= '9') || strchr("_./=:,+@%^-", b))) {
                *p++ = '\\';
            }
            *p++ = *c;
        }
    }
    *p = '\0';
    return out;
}

static int connect_broker(void) {
    const char *name = getenv(SOCKET_ENV);
    if (!name || !*name) name = SOCKET_DEFAULT;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t namelen = strlen(name);
    if (namelen + 1 > sizeof(addr.sun_path)) {
        fprintf(stderr, "ando: socket path too long: %s\n", name);
        return -1;
    }
    memcpy(addr.sun_path, name, namelen + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("ando: socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "ando: broker not running at %s (%s) — is the tawc app alive?\n",
                name, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// A closed std fd would make the SCM_RIGHTS sendmsg fail with EBADF;
// substitute /dev/null so `ando cmd 0<&-` still runs.
static int usable_fd(int fd) {
    if (fcntl(fd, F_GETFD) != -1) return fd;
    int nul = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
    return nul >= 0 ? nul : fd;
}

static int send_fds(int sock, int cwd_fd) {
    char byte = 'F';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    int fds[4] = { usable_fd(0), usable_fd(1), usable_fd(2), cwd_fd };
    union {
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr align;
    } u;
    memset(&u, 0, sizeof(u));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
    ssize_t n;
    do {
        n = sendmsg(sock, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        perror("ando: sendmsg");
        return -1;
    }
    return 0;
}

// Block until the broker's "EXIT <code>" line; return the code.
static int await_exit(int sock) {
    char buf[256];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(sock, buf + have, sizeof(buf) - have - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("ando: read");
            return EXIT_PROTOCOL;
        }
        if (n == 0) {
            fprintf(stderr, "ando: broker closed connection without exit status\n");
            return EXIT_PROTOCOL;
        }
        have += (size_t)n;
        buf[have] = '\0';
        char *nl;
        char *start = buf;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            if (strncmp(start, "EXIT ", 5) == 0) {
                return (int)strtol(start + 5, NULL, 10);
            }
            start = nl + 1;
        }
        size_t rest = have - (size_t)(start - buf);
        memmove(buf, start, rest);
        have = rest;
        if (have >= sizeof(buf) - 1) have = 0;  // garbage line; drop
    }
}

int main(int argc, char **argv) {
    // A broker that dies mid-session must surface as an error message,
    // not a silent SIGPIPE death.
    signal(SIGPIPE, SIG_IGN);

    int opt_preserve_all = 0;
    const char *opt_chdir = NULL;
    const char *opt_user = NULL;
    int opt_shell = 0;
    // -e and --preserve-env=LIST can repeat; argc bounds both counts.
    const char **env_extras = malloc((size_t)argc * sizeof(char *));
    const char **preserve_lists = malloc((size_t)argc * sizeof(char *));
    size_t n_env = 0, n_lists = 0;
    if (!env_extras || !preserve_lists) {
        fprintf(stderr, "ando: out of memory\n");
        return EXIT_PROTOCOL;
    }

    static const struct option longopts[] = {
        { "env", required_argument, NULL, 'e' },
        { "preserve-env", optional_argument, NULL, 'E' },
        { "chdir", required_argument, NULL, 'D' },
        { "shell", no_argument, NULL, 's' },
        { "user", required_argument, NULL, 'u' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    // Leading '+': stop at the first non-option so `ando ls -la` keeps
    // passing -la to ls. `--` still terminates explicitly.
    int c;
    while ((c = getopt_long(argc, argv, "+e:D:u:Ersh", longopts, NULL)) != -1) {
        switch (c) {
        case 'e': {
            // Key must be non-empty and line-safe: only the value half
            // is encoded on the wire, so LF/CR in a key would inject
            // header lines.
            const char *eq = strchr(optarg, '=');
            if (!eq || eq == optarg ||
                memchr(optarg, '\n', (size_t)(eq - optarg)) ||
                memchr(optarg, '\r', (size_t)(eq - optarg))) {
                fprintf(stderr, "ando: -e needs K=V\n");
                return EXIT_PROTOCOL;
            }
            env_extras[n_env++] = optarg;
            break;
        }
        case 'E':
            // --preserve-env=LIST sets optarg; bare -E/--preserve-env
            // never consumes a separate word (sudo parity).
            if (optarg) preserve_lists[n_lists++] = optarg;
            else opt_preserve_all = 1;
            break;
        case 'D': opt_chdir = optarg; break;
        case 's': opt_shell = 1; break;
        case 'u': opt_user = optarg; break;  // repeated -u/-r: last wins
        case 'r': opt_user = "root"; break;
        case 'h': usage(); return 0;
        default:  // getopt_long already printed the complaint
            usage();
            return EXIT_PROTOCOL;
        }
    }

    char **cmd_args = argv + optind;
    int cmd_argc = argc - optind;
    if (cmd_argc == 0 && !opt_shell) {
        usage();
        return EXIT_PROTOCOL;
    }

    if (opt_chdir && chdir(opt_chdir) != 0) {
        fprintf(stderr, "ando: chdir %s: %s\n", opt_chdir, strerror(errno));
        return EXIT_PROTOCOL;
    }

    // -u/-s rewrite the outgoing argv (notes/ando.md "CLI"). The shell
    // is fixed to /system/bin/sh — the guest's $SHELL is a rootfs path
    // that doesn't exist Android-side. su's -c also goes through sh -c,
    // so both routes share the joined string.
    char *joined = NULL;
    if (cmd_argc > 0 && (opt_user || opt_shell)) {
        joined = shell_join(cmd_args, cmd_argc);
        if (!joined) {
            fprintf(stderr, "ando: out of memory\n");
            return EXIT_PROTOCOL;
        }
    }
    const char *rewrite[4];
    const char *const *final_argv;
    int final_argc;
    if (opt_user) {
        const char *su = getenv(SU_ENV);
        if (!su || !*su) su = "su";
        rewrite[0] = su;
        rewrite[1] = opt_user;
        if (joined) {
            rewrite[2] = "-c";
            rewrite[3] = joined;
            final_argc = 4;
        } else {
            final_argc = 2;  // su's default action is an interactive shell
        }
        final_argv = rewrite;
    } else if (opt_shell) {
        rewrite[0] = "/system/bin/sh";
        if (joined) {
            rewrite[1] = "-c";
            rewrite[2] = joined;
            final_argc = 3;
        } else {
            final_argc = 1;
        }
        final_argv = rewrite;
    } else {
        final_argv = (const char *const *)cmd_args;
        final_argc = cmd_argc;
    }

    int cwd_fd = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (cwd_fd < 0 && !opt_chdir) {
        // Deleted cwd etc. — hand over / (the broker child only falls
        // back to the app process's cwd if fchdir itself fails). With
        // -D the dir was just validated; no fallback.
        cwd_fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    }
    if (cwd_fd < 0) {
        perror("ando: open cwd");
        return EXIT_PROTOCOL;
    }

    sock_fd = connect_broker();
    if (sock_fd < 0) return EXIT_NO_BROKER;

    if (send_fds(sock_fd, cwd_fd) != 0) return EXIT_PROTOCOL;
    close(cwd_fd);

    if (write_all(sock_fd, "TAWCANDO 1\n", 11) != 0) goto wfail;
    for (int i = 0; i < final_argc; i++) {
        if (send_line(sock_fd, "ARGV ", final_argv[i]) != 0) goto wfail;
    }
    // ENV order matters: the broker applies lines last-wins, so
    // forwarded env goes first, the TERM default next, -e extras last —
    // explicit -e always beats -E/policy.
    if (opt_preserve_all) {
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            if (!eq) continue;
            size_t klen = (size_t)(eq - *e);
            if (env_blocked(*e, klen)) continue;
            if (forward_env_var(sock_fd, *e, klen, eq + 1) != 0) goto wfail;
        }
    }
    for (size_t i = 0; i < n_lists; i++) {
        // Named vars skip the blocklist (naming is as deliberate as
        // -e); unset names are silently skipped, like sudo.
        char *list = strdup(preserve_lists[i]);
        if (!list) goto wfail;
        for (char *name = strtok(list, ","); name; name = strtok(NULL, ",")) {
            const char *val = getenv(name);
            if (!val) continue;
            if (forward_env_var(sock_fd, name, strlen(name), val) != 0) {
                free(list);
                goto wfail;
            }
        }
        free(list);
    }
    const char *term = getenv("TERM");
    if (term && *term) {
        if (send_line(sock_fd, "ENV TERM=", term) != 0) goto wfail;
    }
    for (size_t i = 0; i < n_env; i++) {
        const char *eq = strchr(env_extras[i], '=');  // validated at parse
        if (send_env_kv(sock_fd, env_extras[i], (size_t)(eq - env_extras[i]),
                        eq + 1) != 0) goto wfail;
    }
    if (write_all(sock_fd, "\n", 1) != 0) goto wfail;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sa.sa_flags = SA_RESTART;  // keep the EXIT read going across signals
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    return await_exit(sock_fd);

wfail:
    fprintf(stderr, "ando: writing request failed (%s)\n", strerror(errno));
    return EXIT_PROTOCOL;
}
