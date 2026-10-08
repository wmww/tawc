# Phone USB link drops during full integration runs

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

Also seen on the OnePlus 9 (LineageOS 21), 2026-10-08, during
`lazy_compositor::test_session_service_survives_hold_churn`: phone logs
`USB_STATE=DISCONNECTED` and adbd `UsbFfs: offline`, adb drops the
suite's forward, the app itself keeps running.

Mitigation: in suite mode the harness (`exec_broker.rs`
`restore_suite_forward`) now waits for the device and re-adds the forward
on a refused connection, so only the test in flight fails instead of
every later one.
