# test_x11_clipboard_text_to_android flaked once (sentinel never landed)

One failure on the emulator (2026-08-10), in the run that also built and
installed the APK — i.e. the first test pass after a cold app start:

    Android clipboard did not become "android clipboard before x11"; last=""

That is the *sentinel* step, before any X11 client starts:
`clipboard-set-text` followed by `clipboard-get-text` reading back "".
An empty read is what Android returns when the app isn't foreground, so
the likely cause is the set/read racing the activity gaining foreground
on a cold start, not the compositor's clipboard plumbing.

Passed on immediate rerun of the same test, of the `x11_clipboard` pair,
and of the whole `xwayland::` module. If it recurs, have the test wait
for the app to be foreground (or retry the sentinel set) rather than
assuming the first `clipboard-set-text` after start sticks.
