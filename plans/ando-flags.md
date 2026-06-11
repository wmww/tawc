# ando sudo-style flags

Grow `ando` (notes/ando.md) from `[-e K=V]… [--] cmd [args…]` to a
sudo-shaped CLI:

```
ando [-E] [--preserve-env[=LIST]] [-D dir] [-s] [-u user | -r]
     [-e K=V]… [--] [cmd [args…]]
```

- `-E, --preserve-env[=LIST]` — forward the guest environment (or just
  LIST, comma-separated) to the Android child, like sudo.
- `-D, --chdir=dir` — start the child in `dir` instead of the caller's
  cwd, like sudo.
- `-s, --shell` — run a shell (`/system/bin/sh`), optionally with a
  command built from the remaining args, like sudo.
- `-e K=V, --env K=V` — unchanged behavior; gains the long form.
- `-u user, --user=user` — run as `user` via Android `su` (requires a
  rooted device / Magisk grant).
- `-r` — alias for `--user=root`.
- `-h, --help` — unchanged; text updated.

## Key fact: client-only change

Everything lands in `tools/ando/src/ando.c`. The broker
(`compositor/src/ando.rs`) and the wire protocol stay at `TAWCANDO 1`
untouched:

- `-E` is just more `ENV` lines (broker already applies arbitrary
  extras; `MAX_HEADER_LINES` 4096 and `MAX_LINE` 64K dwarf any real
  guest env).
- `-D` is a client-side `chdir(dir)` before the existing
  `open(".", O_PATH)` — the cwd-fd design means path translation
  (tawcroot/proot/chroot) is already handled, since the *client*
  resolves the path in guest terms.
- `-s`, `-u`, `-r` are argv rewrites before the `ARGV` lines go out.

Env precedence needs no broker change either: `Command::envs` is a
map insert where the last duplicate wins, so the client controls
precedence purely by send order.

## Parsing rework

The current ad-hoc loop becomes `getopt_long` (bionic's static libc.a
ships it) with a leading `+` in the optstring so parsing stops at the
first non-option — `ando ls -la` and `ando echo -e x` must keep
treating `-la`/`-e x` as the command's args, as today. Keep `--` as an
explicit terminator. Optstring `+e:D:u:Ersh`, long opts
`--env` (required arg), `--chdir` (required), `--user` (required),
`--preserve-env` (optional arg, `=LIST` only — like sudo, a separate
word is never consumed), `--shell`, `--help`. Unknown/malformed
options → usage to stderr, exit 125 (`EXIT_PROTOCOL`), as today.

Post-parse validation:

- A command is required unless `-s` is given (sudo parity: `sudo -u x`
  alone is a usage error; `ando -r` alone → "need a command or -s").
- Repeated `-u`/`-r`: last one wins (getopt convention). `-r` simply
  sets user to `root`.
- `-e` keeps requiring `K=V` (and `--env` likewise).
- `-D` with an empty arg, or `chdir` failure at startup → one error
  line + exit 125. No fallback (the `open("/")` fallback remains only
  for the no-`-D` deleted-cwd case).

## Per-flag design

### -E / --preserve-env

Plain `-E`: walk `environ`, send each var as an `ENV` line, **except a
fixed blocklist**: `PATH`, `LD_PRELOAD`, `LD_LIBRARY_PATH`. Rationale:
guest values of these are rootfs paths that are meaningless or
actively breaking Android-side — guest `PATH` has no `/system/bin`
(would break the broker's child-side PATH search, since std sets the
child environ before `execvp`), and guest `LD_*` is libhybris baggage
pointing at paths the bionic linker can't see. sudo similarly strips
`LD_*` always. An explicit `-e PATH=…` (already supported and tested)
or `-e LD_PRELOAD=…` still works — explicit wins over policy.

`--preserve-env=LIST`: send only the named vars (comma-separated),
blocklist not applied (explicit naming is as deliberate as `-e`).
Unset names are silently skipped, like sudo.

Send order on the wire: `-E`/list env first, then the default `TERM`,
then `-e` extras — so `-e` beats `-E`-forwarded values and the
existing `TERM` default keeps working (with `-E`, environ's `TERM`
arrives first and the default overwrite is a no-op).

Implementation note: drop the fixed `env_extras[256]` cap as part of
this — stream `-e` occurrences in a second getopt pass (or collect
into a growable list); `-E` iterates `environ` directly. A var whose
encoded `ENV` line would exceed the broker's 64K `MAX_LINE` is
skipped with a one-line stderr warning (today it would kill the
connection mid-header).

### -D / --chdir

Early in `main`, after parsing: `chdir(dir)`; on failure
`ando: chdir <dir>: <errno>` + exit 125. The existing cwd-fd logic
then picks it up unchanged. Relative dirs resolve against the
caller's cwd, symlinks/binds resolve through the guest view — all for
free.

Interaction with `-u`: the broker child (the su client) gets the cwd
via `fchdir`; whether Magisk's daemon-spawned root process inherits
that cwd is su's business — verify on the phone and document the
answer in notes/ando.md.

### -s / --shell

The shell is fixed to `/system/bin/sh`. sudo uses `$SHELL`, but the
guest's `$SHELL` is a rootfs path (`/bin/bash`) that doesn't exist
Android-side; document the divergence.

- `ando -s` → argv `["/system/bin/sh"]` (interactive; the usual
  no-job-control pty caveat from notes/ando.md applies).
- `ando -s cmd args…` → argv `["/system/bin/sh", "-c", JOINED]`.

`JOINED` is the sudo-style join: each arg has every byte outside
`[A-Za-z0-9_./=:,+@%^-]` backslash-escaped, args joined with single
spaces. So `ando -s echo 'a b'` runs `sh -c 'echo a\ b'` and prints
`a b`.

### -u / --user, -r

Pure argv rewrite to Android `su`, resolved bare through the broker's
normal PATH search (Magisk mounts `su` into the app PATH — this is
exactly the existing `ando su -c '…'` flow, now spelled `ando -r -s …`
or `ando -u user cmd`):

- `ando -u USER cmd args…` → `["su", USER, "-c", JOINED]` (same
  escaping as `-s`; needed because su hands the string to `sh -c`).
- `ando -u USER -s` (no command) → `["su", USER]` (su's default action
  is an interactive shell, already `/system/bin/sh`).
- `ando -u USER -s cmd…` → same as the non-`-s` form (both routes go
  through `sh -c`).

Magisk's `-c` joins all remaining argv with spaces before invoking
`sh -c`, so passing one pre-escaped string is robust under either
parse. **Verify on the rooted phone** (the `su <user> -c <str>`
ordering vs `su -c <str> <user>`, env propagation with `-E`, cwd
propagation with `-D`) — fold these into the existing
`issues/ando-su-flow-unverified-on-phone.md` checklist rather than a
new issue. No root on the emulator: app-uid `su` is Magisk-only, so
automated tests cannot exercise real su (see Testing).

Failure mode without root: `su` absent → existing broker `127`
"not found" path, message names `su` (which correctly identifies the
missing piece). No special-casing.

### Test hook: TAWC_ANDO_SU

To test the `-u` rewrite without root, the client reads
`TAWC_ANDO_SU` (default `"su"`) for argv[0] of the rewrite — same
pattern and naming as `TAWC_ANDO_SOCKET`, test-only, no production
flow sets it. `TAWC_ANDO_SU=/system/bin/echo ando -u shell id -u`
then prints `shell -c id -u`, asserting the exact constructed argv
from an unrooted test run.

## Interaction matrix (what must hold)

| combo | behavior |
|---|---|
| `-e` vs `-E` | `-e` wins (send order; broker map last-wins) |
| `-E` + `-u` | env lines apply to the su *client*; propagation beyond is su's policy (verify on phone) |
| `-D` + anything | always client-side chdir before cwd-fd open; orthogonal |
| `-s` + `-u` | `su USER -c JOINED`, or `su USER` with no command |
| `-r` + `-u` | last wins |
| `-u`/`-r` without `-s` or command | usage error, 125 |
| options after first non-option arg | belong to the command, never to ando |
| `--` | ends option parsing, next word is the command even if it starts with `-` |

## Testing (tests/integration/tests/ando.rs — no root required)

- **Parsing boundary**: `ando echo -e hi` prints `-e hi`;
  `ando -- -weird-name` fails with the broker's 127 for
  `-weird-name`, not a usage error.
- **-E**: export a guest var, `ando -E sh -c 'echo $V'` sees it;
  guest-exported `LD_PRELOAD`/`PATH` do *not* arrive under `-E`
  (extends the existing env-hygiene test); `-e V=x -E` → `x` wins;
  `--preserve-env=A,B` forwards exactly those.
- **-D**: `ando -D /root sh -c pwd` ends with `/rootfs/root`
  (mirrors the cwd-fd test); relative `-D`; nonexistent dir → rc 125
  with a chdir error on stderr.
- **-s**: `ando -s` with a piped `echo hi` on stdin? — skip
  interactive; instead `ando -s exit 9` → rc 9; escaping:
  `ando -s echo 'a b'` → `a b`; no command + no `-u` runs
  `/system/bin/sh` (covered implicitly by `printf 'exit 5\n' | ando -s`
  if convenient, else leave interactive to the manual pass).
- **-u/-r argv construction** via `TAWC_ANDO_SU=/system/bin/echo`:
  `-u shell id -u` → `shell -c id -u`; `-r id` → `root -c id`;
  `-u a -r` → root wins; `-r -s` (no command) → bare `root`;
  escaping inside the `-c` string.
- **Errors**: unknown option, `-D`/`-u`/`-e` missing args, `-r` with
  no command and no `-s` → all rc 125; `-h` rc 0 and mentions the new
  flags.
- **Real su**: not automatable (root-only); manual phone checklist
  lives in `issues/ando-su-flow-unverified-on-phone.md` — extend it
  with `-u root`/`-r`, `-E`+`-u`, `-D`+`-u`, and interactive
  `ando -r -s`.

Existing tests must keep passing unchanged — notably default env
hygiene (no `-E` → still only TERM + extras) and option-stop behavior.

## Deliverables / order of work

1. `tools/ando/src/ando.c`: getopt_long rework + all flags + usage
   text (keep it terse).
2. Rebuild via `tools/ando/build.sh --abi=both`; the binary rides the
   APK (`jniLibs/<abi>/libando.so`) and `AndoInstallProvider`
   re-stamps `/usr/local/bin/ando` on upgrade — no installer changes.
3. Tests in `tests/integration/tests/ando.rs` per above
   (`scripts/run-integration-tests.sh ando`).
4. Docs: notes/ando.md — new "CLI" section (flags, escaping rule,
   `-E` blocklist, `/system/bin/sh` divergence, `TAWC_ANDO_SU` hook),
   plus updated "Semantics and known limits".
5. Update `issues/ando-su-flow-unverified-on-phone.md` with the new
   root-only verification items.

No broker, protocol, installer, or Kotlin changes anticipated. If
implementation does end up touching the broker (it shouldn't), bump
nothing — the protocol is unversioned-compatible for added ENV lines.
