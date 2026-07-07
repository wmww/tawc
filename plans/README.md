# Plans Index

Future work and speculative implementation plans live here. Keep current-state
design, build, and operational notes in [`../notes/`](../notes/).

- [audio.md](audio.md) - planned PipeWire/PulseAudio bridge to Android audio.
- [gfxstream-bridge-remaining-work.md](gfxstream-bridge-remaining-work.md) - remaining GL/GLES and x86_64 AVD work for the gfxstream bridge backend.
- [gl-on-gles-translator.md](gl-on-gles-translator.md) - possible in-house GL 3.3-core-on-ES 3.2 translator (glslang/SPIRV-Cross shader pipeline) for the modern-GL gap zink can't cover on Vulkan 1.1 devices.
- [tawcroot-landlock.md](tawcroot-landlock.md) - kernel-enforced path containment for tawcroot via Landlock (probe-and-enable, kernel 5.13+).
- [tawcroot-readonly-binds.md](tawcroot-readonly-binds.md) - future read-only fake bind support in tawcroot.
- [verify-libhybris-ahb-alpha.md](verify-libhybris-ahb-alpha.md) - verify sampled-alpha AHB rendering on device after removing the force-opaque workaround.
- [xwayland-server-side-gl.md](xwayland-server-side-gl.md) - parked Xwayland server-side GL acceleration plan.
- [xwayland-glibc-alternative.md](xwayland-glibc-alternative.md) - parked glibc-built Xwayland approach and seccomp patching notes.
