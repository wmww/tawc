# Exec broker: stdin from a file redirect arrives empty

`scripts/rootfs-run.sh 'sh -c "cat > /tmp/x"' < somefile` produces an
empty `/tmp/x` (0 bytes, md5 of empty), even though `tawc-exec`'s
`pump_stdio` reads local stdin unconditionally and
`ExecBrokerSession.streamProcessInner` forwards STDIN frames to the
child. Interactive stdin was not tested; a 1.6 MB binary via `<file`
reliably arrives as 0 bytes (2026-07-06, physical target).

Likely a race or drop between the host sending STDIN frames (+ EOF
immediately, since the file drains instantly) and the in-rootfs child
being wired up — e.g. frames forwarded into a pipe whose write end is
closed on the EOF frame before the child reads, or the tawcroot dispatch
form not inheriting the stdin pipe.

Impact: no way to stream files into the rootfs through the broker;
worked around with `adb push` + `su cp` into
`/data/data/me.phie.tawc/distros/<id>/rootfs/`, which only works on
rooted devices.
