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
- `text_input::test_stale_newline_context_editing_paths`: failed once in
  a full-filter run (exact assertion not captured), passed in every
  other run including the same filter afterwards.

Neither test uses `setComposingText`, so the mid-composition echo skip
(added for the Kate preedit-echo bug) never engages in either — the
flakes reproduce timing races, not that change.

Possible fixes: replace the post-Ctrl+V sleep with a poll of the entry's
content (Ctrl+A/Ctrl+C round-trip) before continuing; capture and file
the stale-newline assertion next time it fires.
