# Emulator-rootless `run-as` chroot subprocess can't reach the internet

`testing/install-test-deps.sh` (and any other host script that does
`tawc-chroot-run` → `pacman -Syu` / `xbps-install` / similar against a
proot or tawcroot install) can't reach a remote mirror on the AVD —
TCP connect to non-on-link IPs times out. Same script works on real
phones; same install-service-driven `pacman -Syyu` works on the
emulator (the production install completes end-to-end). The gap is
specifically "rootless chroot run-as'd from adb shell on the AVD".

## Mechanism

Android does per-app default-route selection through fwmark, not
through the kernel's main routing table. The default route for an app's
preferred network lives only in a per-network table (`wlan0`, `eth0`,
`rmnet_data*`); the main table holds only on-link entries. An `ip rule`
with a netId fwmark + uidrange selects the right per-network table, e.g.
on the AVD:

```
15040: from all fwmark 0x10065/0x1ffff iif lo uidrange 10145-10145 lookup wlan0
```

That's a single-uid range covering the foreground app slot. On the real
phone the equivalent rule has `uidrange 0-2147483647` — every uid is
in range, so any process whose socket carries the right fwmark egresses
fine. AVDs ship the narrow form, presumably because the emulator's
ConnectivityService only registers the active foreground app.

The fwmark itself is stamped onto every `socket()` call by
`libnetd_client.so`'s libc shim, using a netd-side per-process record
established when Zygote forked the app. JVM-spawned subprocesses
inherit that record across fork, so `ProcessBuilder` children of the
install service get a working fwmark. `run-as` is a fresh exec from
adbd: the resulting process has no netd record, the shim stamps zero,
and the rule chain falls through to tables with no default route.

So on the AVD without root the only paths that reach the internet are:

  - children of the app's JVM (install service, anything triggered via
    `am start`/broadcast that lands inside our process), or
  - `su` / uid 0, which has its own carve-out and uses the main table
    via `from all fwmark 0x0/0xffff uidrange 0-0 lookup main`.

## Workarounds today

  - **Rooted AVD (current dev loop):** run pacman/xbps via `su -c
    /…/enter.sh <b64>` instead of `run-as $PKG /…/enter.sh <b64>`. I've
    been doing this manually to install test deps on the emulator.
  - **Real device:** unaffected, no change needed.

## Proposed fix

Add a broadcast (or activity) endpoint to the app that takes a base64
chroot command, runs it through the matching `InstallationMethod`'s
`runInside` (so the subprocess is a JVM child with a netd record), and
streams stdout/stderr back to logcat with a stable tag. Have
`tawc-chroot-run` detect emulator + rootless install method and route
through that endpoint instead of `run-as`. Same dispatch shape as the
existing install/uninstall broadcast surface (notes/installation.md).

Tradeoff: another adb-driven control surface to keep in sync, but it
reuses `InstallationMethod.runInside` which already handles
quoting/setsid/redirect — and it's the only rootless path that actually
egresses on the AVD.

## Out of scope

Emulator's narrow uidrange isn't something we can change from inside
the app — netd's rule installation is driven by ConnectivityService and
the AVD's net policy. Fixing it upstream would mean either invoking
`ConnectivityManager.bindProcessToNetwork(activeNetwork)` on the
spawned subprocess (only possible from Java/Kotlin context, hence the
broadcast plumbing above) or running with CAP_NET_ADMIN to set
`SO_MARK` ourselves (only available with root).

## Related

  - Repro: `bash testing/install-test-deps.sh` against a tawcroot
    emulator install fails at `pacman -Syyu`'s mirror fetch with
    `Could not resolve host` / `Connection timed out`.
  - Adjacent symptom from a different angle:
    [curl#9647](https://github.com/curl/curl/issues/9647) — c-ares in
    a standalone Android process can't resolve hosts. Different code
    path, but the underlying "no netd registration → no working
    sockets" is the same plumbing.
  - Background:
    [Yotam — Network Management in Android: Routing](https://yotam.net/posts/network-management-in-android-routing/),
    [system/netd Fwmark.h](https://android.googlesource.com/platform/system/netd/+/master/include/Fwmark.h),
    [ConnectivityManager.bindProcessToNetwork](https://developer.android.com/reference/android/net/ConnectivityManager#bindProcessToNetwork).
