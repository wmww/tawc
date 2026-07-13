# Usecase tests

One-shot, agent-executed manual test plans. Each file here describes a
realistic end-user usecase that is *expected to work* on the current build.
Each file is assigned to a separate autonomous agent and run without human
help. These are exploratory product tests, not scripted regression tests —
the goal is to catch snags a real user would hit, not to grow the automated
suite (though a test may *suggest* an automated test if it finds something
worth locking down).

## Procedure

1. Read this README fully, then your assigned test file.
2. Follow all repo rules (CLAUDE.md), especially device safety, cache-proxy
   rules, and the `/data/local/tmp/tawc-dev/` scratch policy.
3. Check `.tawctarget`. If your test's **Target** line is incompatible with
   the current target (e.g. physical-only test, target is `emulator` or
   `none`), stop and report — never substitute devices.
4. Assume you need exclusive device access. Do not run alongside other
   usecase-test agents or the integration suite.
5. Tests run against an **Arch Linux tawcroot install** by default; a test
   file may name a different distro when it matters. Confirm what is
   installed with `scripts/rootfs-run.sh 'cat /etc/os-release'` (set
   `TAWC_INSTALL_ID` when more than one distro is installed). If the needed
   distro is missing, install it via the `install` broker action with
   `--arg method=tawcroot` and the cache proxy (see CLAUDE.md and
   notes/exec-broker.md).
6. Package installs inside the rootfs: a proxied install hard-wires the
   mirror config to `http://127.0.0.1:8080` (notes/cache-proxy.md), reached
   from the device via `adb reverse` which the proxy script maintains.
   Before any `pacman -S`, check the proxy from the host:
   `curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/` must
   print `404`. Connection refused means the proxy is down — you are
   blocked (never start it yourself): report and stop.
7. Prefer existing scripts (`scripts/rootfs-run.sh`, `scripts/tawc-exec.sh`)
   over ad-hoc adb. The exec broker has no PTY, so curses/interactive
   programs will not render through `rootfs-run.sh`; drive them inside
   `tmux` or via the broker input actions (notes/exec-broker.md).
8. Where a plan calls for visual verification: screenshot to
   `/data/local/tmp/tawc-dev/`, pull to your host scratchpad, analyze with
   a sub-agent, then delete both copies.

## Outcomes

**Success** — everything behaved as the plan expects:

- Clean up (below).
- Delete the test file.
- Add a one-line entry to the "Completed" list at the bottom of this README.
- Commit (`usecase-tests: <name> passed`).

**Problems** — something broke, diverged from documented behavior, or was
clearly worse than a user would tolerate:

- Debug enough to characterize the failure and which layer owns it
  (distro packaging, tawcroot, compositor, app, docs).
- File a new issue in `issues/usecase_tests/` with details and a repro
  (or extend an existing issue if it is the same bug).
- Update your test file with what happened and the issue reference. Do
  **not** delete it and do **not** add it to the Completed list.
- Code changes are in scope only if simple and minor; otherwise stop at
  the issue.
- Commit.

Either way, commit only this directory, `issues/usecase_tests/`, and (rarely) minor
fixes or note corrections that fell out of the run.

## Cleanup

Leave the device as you found it:

- Remove files you created in the rootfs (`/root/usecase-*`, `/tmp`, …).
- Uninstall packages you installed for the test (`pacman -Rns …`) unless
  removal would destabilize the install; if you leave packages behind, say
  so in the commit message.
- Kill any background processes you started in the rootfs.
- Delete everything you put in `/data/local/tmp/tawc-dev/` and host copies.
- Revert Android-side state you changed (appops grants, ando toggles,
  settings, added binds).

## Completed

(one line per finished test; kept so future test authors don't re-cover
the same ground)

- android-serve-http-to-browser — passed on physical (Arch tawcroot; browser render, tap-through, adb-forward host access, clean shutdown); side finding: issues/usecase_tests/proc-cmdline-masking-breaks-pgrep-pkill-ps.md
- android-shared-storage-binds — passed on physical (Arch tawcroot; ManageBinds UI add/remove/suggestion/RO-dialog, both-direction RW round-trip, RO EROFS enforcement, revoked-grant fail-closed with actionable error + grant banner, live re-grant recovery; last-card-under-add-button issue not reproduced); side finding: issues/usecase_tests/manage-binds-dialog-triggers-password-autofill.md
- cli-background-daemon — passed on physical (Arch tawcroot; nohup daemon survives session exit and ~10 min app-backgrounded, host-side tawc-exec SIGKILL triggers broker descendant kill, kill-by-pid works); pkill/pgrep/ps-by-name still broken per issues/usecase_tests/proc-cmdline-masking-breaks-pgrep-pkill-ps.md (extended with confirmation)
- cli-git-workflow — passed on physical (Arch tawcroot; HTTPS clone of octocat/Hello-World, full local workflow with real merge-conflict resolve, diff, --graph log, annotated tag, gc; git fsck clean; git status over 1003 files returned in ~0.06s — no tawcroot syscall slowdown)
- cli-man-and-docs — passed on physical (Arch tawcroot; matches slimming policy). Fresh state: no `man` binary, `/usr/share/man` and `/usr/share/doc` absent, `man bash` → `command not found`, `bash --help` fully functional. `pacman -S man-db man-pages` installs cleanly (only cosmetic journald/chroot hook warnings), but the `NoExtract = usr/share/man/*` rule blocks ALL pages including man-pages' own — `man bash`/`man 2 open`/`man pacman` all give graceful `No manual entry` (exit 16), no crash/corruption. Power-user recovery works: delete the `usr/share/man/*` NoExtract line + `pacman -S bash man-pages` repopulates `/usr/share/man` and `man bash`/`man 2 open` render real pages. No issue filed — failure modes are sane and documented.
