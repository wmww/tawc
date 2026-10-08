# Pixel 9 Pro USB link drops during full integration runs

In 2 of 4 full `run-integration-tests.sh` runs (2026-10-08) the phone
fell off adb inside `tawcroot::test_tawcroot_device_suite`, both times
in `tawcroot/tests/hosted/test_linkstore_kill`; every later test then
fails with "compositor is not running". The phone logs
`UsbDeviceManager: Usb state update DISCONNECTED` with adbd's pid
unchanged, so the USB link itself reset — nothing killed adbd.

Not reproduced in isolation: `tawcroot/test.sh --device --no-build
tawcroot/tests/hosted/test_linkstore_kill` passed 10/10 (~88 s each),
and the whole device suite passed alone. Suspects: load/thermal after
minutes of GPU tests, the cable/port, or other sessions sharing the adb
server (several were active, one restarted the emulator mid-run).
