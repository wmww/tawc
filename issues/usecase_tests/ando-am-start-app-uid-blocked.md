# `ando am start` fails silently (exit 255) — app uid can't use the activity shell interface

Found by the `android-ando-broker` usecase test on the physical OnePlus
(Android 14, device 50f4ca18), 2026-07-13.

## Symptom

From an enabled Arch tawcroot guest:

```
$ ando am start -a android.intent.action.VIEW -d https://example.com
$ echo $?
255
```

No stdout, no stderr, nothing in logcat. Same for
`ando am start -a android.settings.SETTINGS` and for `cmd activity …`
directly. A user scripting "open a URL from Linux" gets a bare 255 and
no clue why.

## Characterization

- **Not an ando bug.** The identical command run as the app uid through
  the debug exec broker (`scripts/tawc-exec.sh -- /system/bin/sh -c
  'am start …'`) fails the same way, so the failure is platform policy,
  not ando's fd/stdio/exit plumbing (all verified working: `getprop`,
  pipes, exit-code propagation, `-s`, `-E`, `-D`).
- `/system/bin/am` wraps `cmd activity`. `cmd` itself works as the app
  uid (`cmd -l` and `cmd package list packages` both succeed), but any
  `cmd activity …` exits 255 with zero output — ActivityManagerService
  restricts its Binder shell-command interface to root/shell and the
  rejection surfaces only as the result receiver's -1 (= 255).
- **Root path works.** On the rooted phone
  `ando -r am start -a android.intent.action.VIEW -d https://example.com`
  printed `Starting: Intent …`, exit 0, and Firefox came to the
  foreground with example.com loaded (screenshot-verified).

## Impact

The "launch apps/URLs from inside Linux" glue usecase is unavailable to
unrooted users, and the failure mode (silent 255) is hostile. Termux hit
the same wall and ships `termux-am` (a socket to the app process that
calls `Context.startActivity`) instead of the shell tool.

## Possible directions

- Document the limitation in notes/ando.md (`am`/`cmd activity` are
  root/shell-only; use `ando -r am …` on rooted devices).
- Longer term: a supported intent-launch path via the app process
  itself (e.g. an `ando`-reachable helper that has the app call
  `startActivity`), which also sidesteps the shell interface. Note
  Android 10+ background-activity-launch rules would apply when tawc
  isn't foreground; launches driven from a foregrounded tawc terminal
  should be allowed.

## Repro

1. `scripts/tawc-exec.sh --action set-ando --arg installId=arch --arg enabled=true`
2. `TAWC_INSTALL_ID=arch scripts/rootfs-run.sh 'ando am start -a android.intent.action.VIEW -d https://example.com; echo $?'`
   → `255`, no output.
3. Contrast: `… 'ando -r am start …'` → works (rooted device, Magisk
   grant for me.phie.tawc).

## Side note (plan bug, not product)

The test plan's step 4 command `ando -s 'id; echo $USER'` fails with
"inaccessible or not found" — expected: `-s` uses the documented
sudo-style join (notes/ando.md), which escapes `;`/spaces, exactly like
real `sudo -s 'id; echo $USER'`. `ando -s id` and
`ando /system/bin/sh -c 'id; echo $USER'` behave correctly.
