# Plans Index

Future work and speculative implementation plans live here. Keep current-state
design, build, and operational notes in [`../notes/`](../notes/).

- [audio.md](audio.md) - planned PipeWire/PulseAudio bridge to Android audio.
- [desktop-gl-dispatch.md](desktop-gl-dispatch.md) - older desktop-GL dispatcher design, likely superseded by libhybris-zink unless GLES-over-Zink overhead is unacceptable.
- [gfxstream-bridge-remaining-work.md](gfxstream-bridge-remaining-work.md) - remaining GL/GLES and x86_64 AVD work for the gfxstream bridge backend.
- [home-screen-shortcuts.md](home-screen-shortcuts.md) - pin launcher entries to the Android home screen as pinned shortcuts; depends on launcher-entry-management.md.
- [launcher-custom-programs.md](launcher-custom-programs.md) - Terminal=true entries in the native terminal, user `.desktop` dirs, and a minimal in-app `.desktop` editor; depends on launcher-entry-management.md.
- [launcher-entry-management.md](launcher-entry-management.md) - launcher hide/unhide + per-entry menu; foundation for the two plans above, execute first.
- [tawcroot-future-work.md](tawcroot-future-work.md) - deferred tawcroot syscall, `/proc`, and performance work.
- [tawcroot-landlock.md](tawcroot-landlock.md) - kernel-enforced path containment for tawcroot via Landlock (probe-and-enable, kernel 5.13+).
- [tawcroot-readonly-binds.md](tawcroot-readonly-binds.md) - future read-only fake bind support in tawcroot.
- [verify-libhybris-ahb-alpha.md](verify-libhybris-ahb-alpha.md) - verify sampled-alpha AHB rendering on device after removing the force-opaque workaround.
- [xwayland-server-side-gl.md](xwayland-server-side-gl.md) - parked Xwayland server-side GL acceleration plan.
- [xwayland-glibc-alternative.md](xwayland-glibc-alternative.md) - parked glibc-built Xwayland approach and seccomp patching notes.
