# Flaky: text_input::test_full_compose_loop_with_click_in_middle

Fails ~30% of the time on the x86_64 rootless emulator, in isolation as
well as in full runs. First seen 2026-08-09 (during proctitle-fixes
validation) as a single full-run failure; on 2026-08-10 an unmodified
`HEAD` build failed **5 of 16** isolated runs of just this test, so it
is a genuine flake, not a regression of whatever change is in flight.

Panic (captured 2026-08-10):

```
'hello ': "Debug app exited waiting for text 'hello ' with cursor
 (received: [... \"PREEDIT:hello\", \"DONE\", \"PREEDIT\",
 \"TEXT_CHANGED:hello\", \"CURSOR_POS:5\", \"DONE\"])"
```

So the first compose loop and `ic_finish_composing` land fine; the test
then dies at `ic_commit_text(" ")` → `wait_for_text_cursor("hello ")`.
"Debug app exited" means the *wayland test client* went away, not that
the assertion saw wrong text. Failing runs finish in ~0.4s vs ~0.85s
for passing ones, i.e. the client dies early rather than timing out.

The compositor logcat for a run has nothing suspicious in it (normal
startup, `OutputHost dropped` at teardown) — the next step is to
capture the test client's own stderr/exit status, not more compositor
logs.

The test drives an IME compose loop with a mid-compose click
(commit 3031b63), so a timing race between synthetic input and client
lifecycle is the obvious suspect.
