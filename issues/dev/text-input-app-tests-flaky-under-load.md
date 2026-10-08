# Timing-sensitive text-input tests flake under full-filter runs

Two one-off failures observed while running `run-integration-tests.sh
text_input` repeatedly on the physical target (each passed on re-run and
in later full runs):

- `apps::test_gtk4_widget_factory_copy_paste_and_text_input`: Android
  clipboard ended as `"gtk4 widget factory edited"` instead of
  `"gtk4 widget factory paste edited"`. Suspect: the fixed 150ms sleep
  after `Ctrl+V` in `ctrl_key()` racing GTK's async data-offer paste, so
  the subsequent `commitText(" input")` / `deleteSurroundingText(5,0)`
  interleave with the paste landing.
- `text_input::test_stale_newline_context_editing_paths`: fails roughly
  1 run in 10 on the x86_64 rootless emulator too, in isolation as well
  as in full runs. Assertion captured 2026-08-10, always on the emoji
  round of `build_stale_newline_context` (`text_input.rs:1036`):

  ```
  expected "a😀bc\\n" after recommit/newline: Timeout after 5s waiting
  for text 'a😀bc\n' (received: [… "TEXT_CHANGED:a😀bc", "CURSOR_POS:4",
  "DONE", "TEXT_CHANGED:a😀bca😀bc", "CURSOR_POS:8", "DONE",
  "KEY:Return", "TEXT_CHANGED:a😀bca😀bc\\n", "CURSOR_POS:9", …])
  ```

  So the `setComposingRegion(0, 4)` before the re-commit didn't cover
  the existing word: the re-commit *appended* `a😀bc` instead of
  replacing it. Suspect the region call racing the preceding
  `commitText`'s state settling — note the surrogate pair means the
  UTF-16 length (4) differs from the visible length, so an off-by-one
  in the region would truncate rather than append; appending points at
  the region never being applied at all.

Neither test uses `setComposingText`, so the mid-composition echo skip
(added for the Kate preedit-echo bug) never engages in either — the
flakes reproduce timing races, not that change.

Possible fixes: replace the post-Ctrl+V sleep with a poll of the entry's
content (Ctrl+A/Ctrl+C round-trip) before continuing; capture and file
the stale-newline assertion next time it fires.
