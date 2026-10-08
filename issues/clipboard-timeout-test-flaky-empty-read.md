# test_client_clipboard_timeout_does_not_replace_android is flaky

Fails ~1 in 3 on the x86_64 emulator (2026-10-08), in isolation too:

    tests/text_input.rs:1448: non-closing clipboard source should not replace Android clipboard
      left: ""
     right: "android clipboard before timeout"

`clipboard_get_text` reads back "" mid-poll. Same symptom as
[x11-to-android-clipboard-test-flaky-on-cold-start.md](x11-to-android-clipboard-test-flaky-on-cold-start.md)
(an empty read is what Android returns when the app isn't the focused
foreground); not investigated.
