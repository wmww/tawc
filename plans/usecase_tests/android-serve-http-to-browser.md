# Usecase test: serve files from Linux to an Android browser

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical (device must have a browser — physical
preferred).
**Usecase:** a user runs a web app / file server in the rootfs and uses it from a normal Android browser on the same phone. Android apps share the device's network namespace, so `127.0.0.1` in the browser should reach the rootfs server directly.

## Prerequisites

- Cache proxy up if python isn't installed yet
  (`pacman -S --noconfirm python`).

## Steps

1. In `/root/usecase-serve/`, create `index.html` with a distinctive
   title/body plus a second file (`data.txt`) and a subdirectory.
2. Start `python -m http.server 8000 --bind 0.0.0.0` in the background
   (nohup pattern from cli-background-daemon.md; survive session exit).
3. Sanity from inside: `curl -s http://127.0.0.1:8000/` shows the page.
4. Open the Android browser at it:
   `adb shell am start -a android.intent.action.VIEW -d http://127.0.0.1:8000/`.
   Screenshot and verify (per README step 8) the page rendered — the
   distinctive title, and click/tap through to the directory listing or
   `data.txt` if driving taps is practical.
5. From the host: `adb forward tcp:8000 tcp:8000`, `curl` from the host
   machine — a second consumer while the browser is/was connected.
6. Stop the server; confirm the browser now gets a connection error on
   reload (server actually released the port).

## Expected results

- The Android browser renders content served from the rootfs; host
  access via adb forward works; shutdown is clean.

## Known issues / caveats

- If the browser shows nothing, separate layers: in-rootfs curl (server
  ok?) → `adb shell curl 127.0.0.1:8000` if available (device netns ok?)
  → browser. Only the broken layer goes in the issue.

## Cleanup

Kill the server, `adb forward --remove tcp:8000`, remove
`/root/usecase-serve/`, back out of the browser, delete screenshots,
uninstall python if you installed it here.
