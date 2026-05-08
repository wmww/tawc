# Tess's Android Wayland Compositor
Work-in-progress Smithay-based Wayland compositor for Android (and the miscellaneous scripts and libraries needed to make Linux apps work). Aside from this README 100% Claudeslop. And yet it works.

## High-level design
- A linux distro such as Arch Linux Arm is installed into a fake chroot run by `tawcroot` (a custom systrap-based syscall emulator built specifically for tawc — rootless, no ptrace overhead). Debug builds also ship `proot` and real-`chroot`-via-`su` paths for comparison; release builds only ship tawcroot
- Client programs are installed into the rootfs, they are naturally compiled with glibc
- [libhybris](https://github.com/libhybris/libhybris) loads the system EGL driver (compiled with bionic) and provides a Wayland EGL platform for buffer sharing
- Libhybris generally requires the Android ROM to be patched, but [our fork](https://github.com/wmww/libhybris) works on stock Android
- Shared memory also requires some fuckery (an ashmem shim, see notes for details)
- With all this in place, clients can connect to our [Android app](app/) which contains a [Smithay-based Wayland compositor](compositor/)
- It doesn't use glibc, and uses the same stock Android EGL driver to composite client surfaces and render them to an Android app window
