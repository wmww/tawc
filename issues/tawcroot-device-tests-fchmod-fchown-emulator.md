# tawcroot device tests: dropped fchmod/fchown identity tests fail on emulator

`tawcroot/test.sh --device` on the x86_64 emulator: 16 failures, all the
same three steps repeated across suite variants:

```
[FAIL] dropped fchmodat(/dev/null) -> real EPERM/EACCES   rv = 0
[FAIL] dropped fchownat(/dev/null) -> real EPERM/EACCES   rv = 0
[FAIL] dropped fchown(/dev/null fd) -> real EPERM/EACCES
```

The tests drop uids/gids to 994 and expect chmod/chown of root-owned
`/dev/null` to fail with a real EPERM/EACCES; on the emulator they
succeed (rv=0), presumably because the rooted-emulator adb shell
retains capabilities (CAP_FOWNER/CAP_CHOWN) across the uid drop, or
SELinux is permissive there.

Pre-existing as of 2026-07-05 (fails identically with and without the
linkat-fallback change made that day: 1923/16 before, 1951/16 after).
Not seen documented for the physical target; likely emulator-only.

Possible fixes: skip these steps when the test host can still chmod a
root file after the drop (capability probe), or drop caps explicitly
in the test harness before the identity-drop steps.
