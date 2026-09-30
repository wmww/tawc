/**
 * libGL.so shim -- GLX stubs + GLES forwarding via DT_NEEDED.
 *
 * Without this shim the chroot's /usr/lib/libGL.so is libglvnd's dispatcher,
 * which routes GLX calls through /usr/lib/libGLX_mesa.so. Mesa's GLX backend
 * needs a DRI-capable GPU fd and an X server, neither of which exist in our
 * chroot — probes like Firefox's glxtest fail hard, which Firefox interprets
 * as "the GPU is broken" and enters its crash-recovery / safe-mode dialog
 * loop instead of opening a browser window.
 *
 * Similarly GTK/libepoxy probes libGL.so for glXGetCurrentContext to decide
 * whether a GLX context is current; `dlsym` returning a real GLX symbol that
 * in turn aborts is worse than `dlsym` finding a stub that returns NULL.
 *
 * This shim:
 *   - Exports GLX stubs so probes detect "no GLX, use EGL"
 *   - Links against libGL.so.1 (which in /usr/lib/hybris/gl-shims is a
 *     symlink to the libhybris GLES library) via DT_NEEDED, so
 *     `dlsym(handle, "glBindTexture")` resolves GLES symbols through
 *     the dependency chain.
 *
 * The DT_NEEDED is load-bearing and is the whole point of the shim, so
 * scripts/build-libhybris.sh asserts it. Note it is easy to lose: the
 * shim references no symbol from libGL.so.1 at link time (everything
 * is dlsym'd at runtime), so a DT_NEEDED of it needs -Wl,--no-as-needed
 * *before* the -l, or GNU ld's default --as-needed drops the library.
 * Without it dlopen succeeds and every GLES dlsym returns NULL.
 */
