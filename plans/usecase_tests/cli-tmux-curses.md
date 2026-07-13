# Usecase test: tmux, vim, and curses apps

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a terminal power user lives in tmux with vim and htop.

The exec broker has no PTY, so this test drives everything through a
detached tmux server — which is itself the interesting part: tmux
exercises guest pty allocation (`/dev/ptmx` through the `/dev` bind), a
unix control socket in `/tmp`, and session persistence.

## Prerequisites

- Cache proxy up (README step 6).

## Steps

1. `pacman -S --noconfirm tmux vim htop`.
2. `tmux new-session -d -s uc` — then all interaction is
   `tmux send-keys -t uc '<keys>' Enter` + `tmux capture-pane -pt uc`
   (each via `scripts/rootfs-run.sh`; the tmux server persists between
   broker invocations since each is a fresh shell).
3. Shell-in-tmux sanity: run `echo hi`, capture, confirm output and a
   prompt.
4. vim: open a new file, enter insert mode, type a couple of lines,
   `Esc :wq`, then `cat` the file to verify contents survived.
5. htop: launch, capture-pane, confirm it drew a process table (header
   row, at least a few processes — you should see bash/tmux themselves),
   then `q`.
6. Persistence: detach is implicit; from a *new* `rootfs-run.sh` call,
   `tmux attach -d -t uc` inside `tmux capture-pane` terms — i.e. verify
   `tmux ls` still shows the session and capture still works minutes
   later.
7. Split a window (`tmux split-window`), run different commands in both
   panes, capture and verify both rendered.

## Expected results

- tmux server starts, survives across broker sessions, and all
  send-keys/capture-pane round trips reflect correct terminal contents.
- vim edits save correctly; htop renders a plausible process list.

## Known issues / caveats

- `ps`/htop see only processes under the app uid — a short list is
  correct, an empty one is not.
- If tmux can't create its socket, look at `/tmp` handling
  (`XDG_RUNTIME_DIR=/tmp` in the rootfs env) before blaming tmux.

## Cleanup

`tmux kill-server`, remove test files, `pacman -Rns tmux vim htop`.
