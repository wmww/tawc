# build-libhybris-aarch64 hides `make install` failures behind `|| true`

`client/build-libhybris-aarch64` runs `make install ... || true` on the
common subdir and every per-component subdir (lines 254-260 area). If
an install step legitimately fails, the script continues to the verify
step where the missing files trip the `MISSING="..."` check — so the
user sees "missing built libraries: …" rather than the actual install
error.

The `|| true` is there because per-subdir installs can fail on subdirs
we don't use (legacy bionic-linker plugin variants for Android 6/7/8).
Tighten this: install only the dirs we actually iterate (`DIRS`), drop
`|| true`, and let real failures propagate. The verify step then
becomes a sanity check rather than the only failure detector.
