# Plans Index

Future work and speculative implementation plans live here. Keep current-state
design, build, and operational notes in [`../notes/`](../notes/).

- [ando-flags.md](ando-flags.md) - sudo-style flags for the ando client (`-E`, `-D`, `-s`, `-u`/`-r`); client-only change plus tests.
- [audio.md](audio.md) - planned PipeWire/PulseAudio bridge to Android audio.
- [desktop-gl-dispatch.md](desktop-gl-dispatch.md) - older desktop-GL dispatcher design, likely superseded by libhybris-zink unless GLES-over-Zink overhead is unacceptable.
- [gfxstream-bridge-remaining-work.md](gfxstream-bridge-remaining-work.md) - remaining GL/GLES and x86_64 AVD work for the gfxstream bridge backend.
- [integration-test-host-transport.md](integration-test-host-transport.md) - plan to remove per-test adb/logcat/tawc-exec process churn from integration tests.
- [tawcroot-future-work.md](tawcroot-future-work.md) - deferred tawcroot syscall, `/proc`, and performance work.
- [tawcroot-readonly-binds.md](tawcroot-readonly-binds.md) - future read-only fake bind support in tawcroot.
- [terminal-tabs.md](terminal-tabs.md) - replace the terminal's toolbar with a compact tab bar; multiple shell sessions per distro, OSC-title tab names.
- [verify-libhybris-ahb-alpha.md](verify-libhybris-ahb-alpha.md) - verify sampled-alpha AHB rendering on device after removing the force-opaque workaround.
- [xwayland-server-side-gl.md](xwayland-server-side-gl.md) - parked Xwayland server-side GL acceleration plan.
- [xwayland-glibc-alternative.md](xwayland-glibc-alternative.md) - parked glibc-built Xwayland approach and seccomp patching notes.
