# client/start-emulator hardcodes emulator-5554 and x86_64-only pgrep

`client/start-emulator` assumes the AVD always lands on serial
`emulator-5554` (lines 82, 90, 98, 103, 111, 118, 121, 123 — every
`adb -s emulator-5554 ...` call). The detection at line 43 matches by
AVD name regardless of port, so if a different AVD is already on
:5554 the script will run all post-boot setup steps against the
wrong one.

The same `pgrep` at line 43 only matches `qemu-system-x86_64`. The
project currently only supports x86_64 hosts (per notes/building.md
line 9), but on aarch64 hosts the AVD launches `qemu-system-aarch64`
and the "AVD already running?" check would silently always say no.

Fix: capture the actual port from the running qemu cmdline (or from
`adb devices` filtered by AVD name via `adb -s <serial> emu avd
name`) and use that throughout. Broaden the pgrep regex to
`qemu-system-(x86_64|aarch64)`.
