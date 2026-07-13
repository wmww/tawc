# Usecase test: Python, venv, and pip

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user does Python scripting/dev: install Python, make a venv, pip-install from PyPI, talk to the network.

## Prerequisites

- Cache proxy up (README step 6) for the pacman install.

## Steps

Work under `/root/usecase-py/`.

1. `pacman -S --noconfirm python python-pip` (via `scripts/rootfs-run.sh`).
2. `python -m venv venv`, activate it in the same `bash -lc` invocation
   (each `rootfs-run.sh` call is a fresh shell — chain with `&&` or use
   the venv's absolute bin paths).
3. `pip install requests` — note pip talks to PyPI **directly** (only
   distro mirrors go through the cache proxy); this checks DNS
   (resolv.conf is 8.8.8.8) and TLS (ca-certificates).
4. Run a script that: GETs `https://example.com` with requests and prints
   the status code; round-trips a dict through `json`; runs
   `subprocess.run(["uname", "-a"])`; writes/reads a file.
5. Quick REPL sanity through the no-PTY broker: `echo 'print(6*7)' | scripts/rootfs-run.sh python`.

## Expected results

- venv creation, pip install, and the script all succeed; HTTPS GET
  returns 200.

## Known issues / caveats

- If TLS fails, check whether `ca-certificates` made it through rootfs
  slimming before blaming the network stack.
- No PTY over the broker: the interactive REPL will look dumb (no
  readline); that is a documented broker limitation, not a bug
  (notes/exec-broker.md "What's not yet done").

## Cleanup

Remove `/root/usecase-py/`; `pacman -Rns python-pip` (leave `python` if
other tests might want it — note it in the commit either way).
