# Usecase test: `ando` — Android commands from inside Linux

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a Linux-side user scripts against Android: query properties, launch apps/URLs, glue the two worlds.

Read notes/ando.md first. ando is per-distro, default **off**,
fail-closed.

## Prerequisites

- A READY Arch tawcroot install; know its install id.

## Steps

1. Fail-closed check first: with ando disabled (default), run
   `scripts/rootfs-run.sh 'ando getprop ro.build.version.release'` —
   must refuse with instructions on enabling, not hang or half-work.
2. Enable via the broker (in-memory override, self-cleaning on app
   death): `scripts/tawc-exec.sh --action set-ando --arg installId=<id> --arg enabled=true`.
3. `ando getprop ro.build.version.release` — prints the Android version.
4. `ando -s 'id; echo $USER'` — runs as the app uid outside the rootfs
   env (contrast with `id` inside the rootfs, which reports fake root).
5. Open a URL: `ando am start -a android.intent.action.VIEW -d https://example.com`
   — verify with a screenshot that an Android browser came to the
   foreground with the page (per README step 8). If the device has no
   browser, `-d https://` handling failing is a device gap; substitute
   `ando am start -a android.intent.action.VIEW -d about:blank` reasoning
   or open Settings (`ando am start -a android.settings.SETTINGS`) and
   note the substitution.
6. Pipe integration: `ando getprop | grep ro.product` from inside the
   rootfs — stdio plumbing through the broker should behave like a
   normal pipe.
7. Disable again (`--arg enabled=false`) and re-verify refusal.

## Expected results

- Disabled → clean refusal; enabled → all commands work with correct
  stdio/exit codes; URL/intent launch brings the target app forward.

## Known issues / caveats

- The `set-ando` override is in-memory only (cleared by app process
  death) — if results look stale, check whether the app restarted
  between steps.

## Cleanup

`set-ando … enabled=false`; return the device to the tawc app or home
screen (back out of the browser/settings you opened); delete
screenshots per README.
