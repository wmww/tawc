# `input::test_input_dispatch` is flaky on the emulator (~20% pass rate)

After `ac87c0b` consolidated the input scenarios into a single chained
`#[test]` that drives one debug-app instance through 13 scenes, the
suite passes consistently on the OnePlus 9 device and intermittently on
the x86_64 emulator (5-run sample: 1 PASS, 4 FAIL).

The flake survives a `am force-stop me.phie.tawc` + sleep before each
run, so it isn't compositor warm-up. It is also independent of the
install method (reproduced on a proot install on emulator; the original
chroot install would hit the same compositor-side text-input pipeline).

## Symptoms

Failure modes seen across the 5-run sample:

1. `scene_full_compose_loop_with_click_in_middle` (most common) —
   ```
   'hello world': "Timeout waiting for text 'hello world' (last: Some(\"hello worlworld\"))"
   ```
   The 5x `setComposingText("w" → "wo" → "wor" → "worl" → "world")`
   loop ends with `"worl" + "world"` in the buffer instead of just
   `"world"`. Looks like the `"worl"` preedit got committed (rather
   than replaced) before `"world"` arrived.

2. `scene_basic_input_and_delete` (less common) —
   ```
   text 'hello world': "Timeout waiting for text 'hello world' (last: None)"
   ```
   The very first `adb::input_text("hello world")` doesn't reach the
   debug app within the 5s `TIMEOUT`.

3. `scene_click_during_preedit_commits_pending_text` —
   ```
   pending preedit 'hello' was dropped on cursor move; got "world"
   ```
   Tap during active preedit doesn't commit the preedit before the
   touch reaches the client.

All three failures are consistent with a race between rapid-fire
`am broadcast` invocations and the compositor's text-input-v3 state
machine. On the slower emulator the broadcasts queue / overlap
differently; on the device they don't.

## Repro

```
TAWC_TARGET=emulator bash testing/run-integration-tests.sh --no-build "input::"
```

Each `scene_full_compose_loop_with_click_in_middle` failure pinpoints
`tests/input.rs:459` — `app.wait_for_text("hello world", TIMEOUT)`.

## Hypotheses

- `am broadcast` from rust-test-side `Command::new("adb").args(...)` is
  effectively async on the receiving side: the receiver may not have
  finished processing broadcast N before broadcast N+1 arrives.
  Compositor's text-input-v3 hops between threads (broadcast receiver
  on main thread → calloop → wayland send) and a faster-than-state
  arrival can squash an in-flight preedit replacement.
- `reset_buffer` between scenes only zeroes the visible buffer; the
  compositor's `current_preedit` / `composingRegionIsPreedit` mirror
  state can carry over and bias the next scene's first interaction.
- Emulator is single-CPU under load (gradle + cargo + emulator + adb
  on the same box), so timing margins shrink.

## Possible directions

- Add a small inter-broadcast settle delay inside the `for prefix in [...]`
  loops (cheap; would mask the race for tests but not fix the
  underlying compositor bug).
- Make `reset_buffer` emit an explicit compositor state-clear and
  wait for the round-trip before returning.
- Split the chained scenario back into per-scene tests; consolidation
  was for GTK startup cost — splitting only the IC-side scenes would
  isolate the race without re-introducing 14× cold starts.
- Investigate whether the compositor's `commit_string` / preedit
  replacement is actually atomic against rapid `set_preedit_string`
  arrivals.
