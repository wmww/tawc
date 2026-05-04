# testing/run-integration-tests.sh uses relative paths after a one-time `cd`

`testing/run-integration-tests.sh` does `cd "$ROOT_DIR"` early (around
line 91) and then references several paths relative to that cwd:
- `server/app/build/outputs/apk/debug/app-debug.apk` (~line 94)
- `testing/tawc-pidfile-exec` (~line 113)
- `tawcroot/build`, `tawcroot/build-fixtures` (~lines 125-126)

`set -e` means a re-ordering / refactor that breaks the cwd assumption
fails loudly, but using `$ROOT_DIR/...` prefixes everywhere would be
more robust and consistent with the rest of the file (which already
uses `$ROOT_DIR/...` in many other call sites).
