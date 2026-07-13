# Plans Index

Future work and speculative implementation plans live here. Keep current-state
design, build, and operational notes in [`../notes/`](../notes/).

- [audio.md](audio.md) - planned PipeWire/PulseAudio bridge to Android audio.
- [gfxstream-bridge-remaining-work.md](gfxstream-bridge-remaining-work.md) - remaining GL/GLES and x86_64 AVD work for the gfxstream bridge backend.
- [gl-on-gles-translator.md](gl-on-gles-translator.md) - possible in-house GL 3.3-core-on-ES 3.2 translator (glslang/SPIRV-Cross shader pipeline) for the modern-GL gap zink can't cover on Vulkan 1.1 devices.
- [tawcroot-default-binds-ro.md](tawcroot-default-binds-ro.md) - make tawcroot's built-in binds read-only where the guest never writes (system partitions; app-asset copy→bind revert).
- [tawcroot-landlock.md](tawcroot-landlock.md) - kernel-enforced path containment for tawcroot via Landlock (probe-and-enable, kernel 5.13+).
- [x86-box64.md](x86-box64.md) - per-program x86_64 emulation in existing arm64 distros via box64 (native-library wrapping; 16K-page hedge).
- [x86-fex-glibc.md](x86-fex-glibc.md) - FEX installed inside an arm64 glibc distro; x86_64 RootFS; tawcroot exec dispatch; whole-x86-distro stretch.
- [x86-fex-bionic.md](x86-fex-bionic.md) - speculative bionic-built FEX with no glibc layer, thunking straight to NDK graphics; gated on the glibc FEX plan.
