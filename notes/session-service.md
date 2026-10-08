# Session service

**A notification exists exactly when there is something in a rootfs to
lose, and it says what.** While it exists the process is a foreground
service: not cached, so no trim/LMK-first kill (which takes every guest
with it — they live in the app's cgroup), no cached-apps freezer, no Doze
firewall cutting guest network. When the last reason goes, service and
notification go. TAWC's own UI with nothing running shows nothing;
installs keep their own `InstallationService` notifications.

Code: `app/src/main/java/me/phie/tawc/session/`.

## Holds

`SessionHolds.acquire(reason): Hold` — process-wide, any thread;
`Hold.release()` is idempotent, `Hold.update(reason)` swaps what it
reports. The registry itself never touches Android (JVM-unit-tested);
`SessionService.install` (from `TawcApplication.onCreate`) gives it the
`startForegroundService` starter.

| Reason | Acquired | Released |
|---|---|---|
| `Terminal(distroId)` | `TerminalSessions.add` (a tab opens) | `remove` / `removeAll` |
| `Command(label)` | around the process in `UserRootfsSession.startInside` (launcher headless launch, `RunCommandOp`, broker `RUNINSIDE`) | a waiter thread on process exit |
| `Compositor(windowCount)` | `CompositorService`, when `nativeStartCompositor` spawned a thread | `onCompositorStopped` |
| `Remote(distroId, clients)` | `RemoteSession.start` (remote access, [remote-access.md](remote-access.md)); client count updated from the agent's status | the agent ending (Stop, TTL, failure, Exit, uninstall) |
| `Stray(count)` | never acquired; service-internal | — |

Terminal holds live in `TerminalSessions`, not the pane — sessions
outlive it. The home screen's *pending* shell (auto-spawned, nothing
typed yet) holds nothing on purpose: opening the app must not start a
foreground service (notes/terminal.md "Session model"). Command holds follow the `Process`, so no caller cooperates.

## Service

`SessionService`: FGS type `specialUse` (subtype `linux_session`),
`START_NOT_STICKY` (after a process kill every guest is dead, and a
sticky restart would call `startForeground` from the background).
`onCreate` is trivial on purpose: `startForegroundService` allows ~5 s to
reach `startForeground`. `onStartCommand` calls `startForeground` again
every time — each `startForegroundService` must be answered.

Start/stop is atomic with the registry: the service stops only through
`SessionHolds.serviceStopIfIdle()`, and an `acquire` that finds no live
service calls the starter, so a release-then-acquire race either keeps
the service or starts a new one.

Every acquire site normally runs while a TAWC activity is visible. The
debug broker without `--foreground-app` is the exception: Android 12+
throws `ForegroundServiceStartNotAllowedException`; it is logged once and
the spawn carries on unprotected (the next acquire retries).

**Stray tail.** A `nohup`/`setsid` job outlives its tab and holds nothing.
When the last hold releases, the service runs `ProcessScanner.scan`
off-thread; if guests remain it stays up as "N background processes" and
re-scans every 15 s until none do (Exit triggers an immediate re-scan
once its kill pass is done). Only this tail state polls.

**Notification.** Channel `tawc_session` (the old `tawc_compositor`
channel is deleted), low importance, ongoing. Title "TAWC running", text
e.g. "2 terminals · 3 windows", "Running: htop", "3 background
processes". Tap opens `MainActivity` on its last pane. Swiping the
home screen's recents card hangs up its shells (`onTaskRemoved`,
[terminal.md](terminal.md) "Swipe = closing the windows"); what they
leave behind shows as background processes.

**Exit** (`SessionExit.killEverything`) kills everything: finishes every
terminal session (tabs close through the normal
`onSessionFinished` path), stops the compositor, and
`ProcessScanner.killAllInRootfs` for every install — except installs that
are not `READY` or have a live `install:`/`uninstall:` operation, whose
processes belong to the installer. One notification stands for every
reason, so a partial exit would leave it up. Holds are not force-released;
each follows its own process down. The kill pass rescans (8 × 250 ms) for
processes forked while dying, but stops once anything new acquires a hold
(`SessionHolds.acquisitions`), so a program started right after Exit
survives.

## Keep awake

The FGS keeps the process alive, not the CPU: screen off and unplugged,
the SoC suspends and every guest stops mid-syscall. Measured on the
OnePlus 9 (2026-09-28): a 1 s rootfs ticker went to bursts with 3–45 s
gaps and 79 kernel suspends over ~15 min. With the lock held: 0
suspends, no tick gap over 2 s, and 122/122 curls over Wi-Fi OK across ~21
min. USB-attached never suspends (`a600000.ssusb` and the charger hold
kernel wakeup sources), so measure unplugged: leave a ticker writing to a
file in the rootfs and read it back after replugging. Compare
`/sys/power/suspend_stats/success` (root) before and after.

Manual, off by default, Termux-style. Holding the CPU for an idle shell
costs battery, and only the user knows whether the job matters.
`SessionWake` holds the state (main thread only). While on,
`SessionService` holds a non-reference-counted `PARTIAL_WAKE_LOCK`
`tawc:session` and a `WIFI_MODE_FULL_LOW_LATENCY` Wi-Fi lock. Android
applies low-latency mode only in the foreground with the screen on, and
`FULL_HIGH_PERF` is a no-op from API 34, so the Wi-Fi lock does little.
The CPU lock is what keeps the network up. The lock never outlives the
service. It is dropped on toggle-off, on Exit, and when the service stops,
and each new service lifetime starts released. Nothing is persisted.

Toggles: a second notification action ("Keep awake" / "Release
wakelock"; the text gains "· awake") and a checkable "Keep awake" in the
in-use terminal's ⋮ menu (shown only while the service is up). The first
enable while TAWC is battery-optimized offers, once
(`Settings.batteryPromptShown`), the system's battery-optimization list
(`ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS`). The direct
`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` dialog would need a
Play-restricted permission.

## Debug surfaces

Broker actions `session-state` (one line per held reason),
`session-exit` (what the Exit button does) and `session-wake [--arg
wake=on|off]` (prints `held`, `released` or `unavailable`). Covered by
`lazy_compositor::test_session_holds_*` and
`test_session_wake_follows_toggle_and_exit`.

## Start/stop must share the main thread

`startForegroundService` obliges the service to call `startForeground`;
if the service is stopped with such a start unanswered, Android throws
`ForegroundServiceDidNotStartInTimeException` and kills the process —
every guest with it, i.e. the exact failure this service exists to
prevent. A start issued from another thread can land between "nothing is
held" and `stopSelf()`. So the starter always runs on the main thread
(posting if needed), and the stop path has no suspension point between
`serviceStopIfIdle` and `stopSelf()`. `SessionHolds` tracks the live
instance by token so a late `onDestroy` of the old instance cannot
unregister its successor. This bit once: a full integration run died at
the first command that raced the compositor's auto-stop releasing its
hold (`lazy_compositor::test_session_service_survives_hold_churn` is the
best-effort guard; it is timing-dependent and did not reproduce the crash
on its own).

## Measured (OnePlus 9, Android 14, 2026-09-20)

Before: a terminal-only session had no service at all; HOME then a few
other apps → `procState=16`, adj 910, and `am kill me.phie.tawc` took
every guest with it. About a minute after screen-off, light Doze's
`fw_dozable` chain cut all guest network (guests run as the app uid):
DNS-shaped failures, curl exit 6, unreproducible while watching because
the in-use terminal keeps the screen on.

After, with a rootfs command running and the app behind three others:
`procState=4` (FGS), adj 50, `am kill` a no-op, a 1 s ticker unbroken. A
detached guest curl loop (held by the stray tail) through forced light
then deep idle, screen off: 48 requests, 0 failures, `procState=4`
throughout. Stray tail: "1 background process" while a `setsid sleep`
lived, service gone within 9 s of it exiting.

Emulator (API 36, where the cached-apps freezer is on): a backgrounded
ticker ran ~40 s with no gap over 2 s, `isFrozen=false`, `procState=4`.

Not measured: a `Terminal` hold specifically (needs the terminal UI; same
service either way), and swiping a `CompositorActivity` card with a
terminal alive.

An FGS is necessary but not always sufficient: a RESTRICTED standby
bucket, user "restrict battery usage", or an aggressive OEM ROM can still
cut the uid, and the app is not on the device-idle allowlist
(`REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` is the lever). It does nothing
for the phantom-process killer or CPU sleep — see "Phantom process
killer" and "Keep awake" below.

## Phantom process killer

Android 12+ trims app-forked processes ("phantom processes") down to
`max_phantom_processes` — 32 on the OnePlus 9 (Android 14), and the cap
is **global across all apps**. tawcroot guests are plain children of the
app process, so every guest counts. No in-app fix exists: the app can't
raise its cap, and an FGS doesn't exempt it.

Measured 2026-08-11: 45 CPU-active guest children → 55 kills
(`ActivityManager: Killing PhantomProcessRecord {…}: Trimming phantom
processes`), whole session wiped. The first victim was the bash of an
*unrelated* terminal tab, and a `su` of another app (u0a247) also died.
So a heavy job in one tab can kill another, or another app.

It is intermittent: AMS only finds phantoms when `ProcessCpuTracker`
samples `/proc`, and only processes with measurable CPU. 60 idle `sleep`s
were never tracked; in a later run 45 busy children went untracked for
over a minute (`dumpsys activity processes`: zero `PhantomProcessRecord`).
`dumpsys cpuinfo` did not force a sweep. Expect "parallel build / package
upgrade fails *sometimes*".

User workarounds (unverified on the target; which sticks varies by
Android version, and `device_config` values reset on config sync or
reboot; Android 14+ also has the first as Developer options → "Disable
child process restrictions"):

```
adb shell settings put global settings_enable_monitor_phantom_procs false
adb shell device_config set_sync_disabled_for_tests persistent
adb shell device_config put activity_manager max_phantom_processes 2147483647
```
