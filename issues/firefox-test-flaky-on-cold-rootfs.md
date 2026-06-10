# `hybris::test_firefox_renders_via_ahb` flakes on the very first run after a fresh install

**2026-06-09 addendum:** unrelated to this flake, both firefox tests
went permanently red via a different mechanism: repeated hard kills
pushed `toolkit.startup.recent_crashes` past 3, making every launch
silently relaunch into safe mode, and the relaunch then wedged on a
tawcroot exec_lock leak. Both fixed (tawcroot prepare/commit split;
`helpers::firefox_profile_cleanup` pins
`toolkit.startup.max_resumed_crashes=-1`) — see
notes/firefox.md "Startup-crash safe-mode relaunch". If this issue's
cold-rootfs flake reappears, rule those out first.

**Likely fixed as of 2026-05-02.** The test's steady-state
assertion was rewritten from "saw a `wlegl: imported` log line in
the last 2 s" (which is empty whenever Firefox's WebRender ring of
2-6 AHBs has settled — i.e., always except the first ~half-second)
to "compositor state shows ≥1 AHB-attached surface and the frames
counter is advancing." The new assertion catches the same
regressions (SHM fallback, wedged client, never-actually-AHB) but
isn't sensitive to import-line frequency. The cold-rootfs flake
described below was almost certainly the same blind spot — Firefox
finishes profile init, settles its buffer ring, and the old test
saw zero new imports. Re-test under proot to confirm and close
this issue if it stays green; the rest of this file describes the
original symptom for archaeology.

---

The Firefox integration test reliably **fails on the first run**
following a fresh proot install + integration-test package setup, then
**passes on every subsequent run** within the same chroot lifetime.

## Repro

```
TAWC_TARGET=physical scripts/tawc-exec.sh --foreground-app --action uninstall --arg id=arch
TAWC_TARGET=physical scripts/tawc-exec.sh --foreground-app --action install --arg id=arch --arg method=proot
TAWC_TARGET=physical scripts/run-integration-tests.sh
# -> hybris::test_firefox_renders_via_ahb FAILS

TAWC_TARGET=physical scripts/run-integration-tests.sh --no-build
# -> ALL PASS, including the firefox test
```

## What's likely happening

Firefox's first launch in a fresh profile dir does a bunch of
one-time setup: extension installation, telemetry/dconf/GSettings
priming, font cache build, dconf-write back to disk, places.sqlite
init. Under proot every syscall is a ptrace stop, so this overhead
amplifies. The test's `wait_for_hardware_buffer` timeout is tuned
for a warm-cache Firefox start (~5-10s) and doesn't accommodate the
~30-60s a cold-start firefox profile build takes.

## Possible directions

- **Detect cold-start and lengthen the timeout once.** Add a "if
  `~/.mozilla/firefox/*` is empty, expect first-run latency" branch.
- **Pre-warm in `run-integration-tests.sh`.** Run `firefox --headless
  --screenshot /tmp/_warm.png about:blank` once after the pacman
  install so test runs always hit a warm profile. Cleaner for the
  test, costs ~30s of one-time setup. Cleanest fix and the one I'd
  pick.
- **Increase the test's blanket timeout.** Cheap but masks future
  regressions where Firefox slowness is real.

## Why we didn't fix it inline

Spotted while testing the proot install end-to-end. The first run
fails reliably and reruns succeed; not a real product bug, but a
test-rig bug worth filing so cold-start CI runs don't get the same
red herring.

## References

- `tests/integration/tests/hybris.rs::test_firefox_renders_via_ahb`
- `scripts/run-integration-tests.sh` (where a pre-warm step would land)
