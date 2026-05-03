# `apps::test_firefox_launches_with_hardware_buffers` flakes on the very first run after a fresh install

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
following a fresh proot install + `install-test-deps.sh`, then
**passes on every subsequent run** within the same chroot lifetime.

## Repro

```
adb shell am start -n me.phie.tawc/.install.UninstallActivity --es autoStart true --es id arch
# wait for uninstall
adb shell am start -n me.phie.tawc/.install.InstallActivity --es autoStart true --es id arch --es method proot
# wait ~6 min for install
TAWC_TARGET=device bash testing/install-test-deps.sh
TAWC_TARGET=device bash testing/run-integration-tests.sh --no-build
# -> apps::test_firefox_launches_with_hardware_buffers FAILS

TAWC_TARGET=device bash testing/run-integration-tests.sh --no-build
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
- **Pre-warm in `install-test-deps.sh`.** Run `firefox --headless
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

- `testing/integration/tests/apps.rs::test_firefox_launches_with_hardware_buffers`
- `testing/install-test-deps.sh` (where a pre-warm step would land)
