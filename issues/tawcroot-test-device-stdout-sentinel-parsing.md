# tawcroot/test --device parses an `__exit=$?` sentinel from stdout

`tawcroot/test --device` runs the cleat orchestrator on-device via
`adb shell su -c "...; echo __exit=\$?"` and greps the trailing
sentinel out of mixed stdout (~lines 154-161). This is exactly the
"adb-grep heuristic" that the file's own header (~line 81) claims
to have eliminated.

A modern adbd preserves the child exit status through `adb shell -t`
(or with `set -o pipefail` and proper sub-shell layering), so the
sentinel could be replaced with the real exit code and the regex
parsing dropped. Keep the sentinel as a fallback for older adbd if
needed, but prefer the real rc.
