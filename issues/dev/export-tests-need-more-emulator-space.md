# distro_export tests don't fit on the emulator next to `arch`

`TAWC_EXPORT_TESTS=1 … distro_export::` on the emulator (2026-10-08):
with the standing `arch` install at 4.0 GB of the 5.9 GB data
partition, `test_distro_export_import` passes everything up to the
truncated-archive import, which then fails with `ENOSPC` instead of
the expected truncation error (SRC + IMP + the half import don't
fit). The same run's `test_custom_distro_import` failed at the Alpine
login shell with `/bin/sh: can't fork: Function not implemented`; not
investigated, and it may or may not be the disk (it pulls Alpine
`latest-stable`, so a new busybox using an unhandled syscall is the
other candidate).

A failing run also leaves its slots behind (`exptest-*`,
`custtest-*`), filling the disk for the next run; remove them with the
broker's `uninstall` action.

`tawc-rootless`'s config asks for a 24 GB data partition, but its
`/data` is still 5.9 GB: the existing userdata image was never grown.
Getting the space needs a data wipe (and a fresh `arch` install).
