# test_composing_region_replacement_paths is flaky

Seen 2026-06-09 on the emulator during an unrelated clipboard run:
`text_input::test_composing_region_replacement_paths` failed with
"setComposingText over region did not delete original bytes"
(tests/integration/tests/text_input.rs:718), then passed twice in a row
when run alone with `--no-build`.

The failing step taps mid-text, waits for a cursor change, then does
`setComposingRegion(0, cursor)` + `setComposingText("HELLO")`. The
assertion inspects `last_text()` immediately after the preedit arrives,
so a stale `last_text` snapshot or a tap landing on an unexpected
cursor position could both explain it. Not reproduced in isolation;
likely timing-sensitive only under full-suite load.
