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
 *   - Exports GLX stubs (return NULL) so probes detect "no GLX, use EGL"
 *   - Links against libGL.so.1 (which in /tmp/gl-shims is a symlink to the
 *     libhybris GLES library) via DT_NEEDED, so `dlsym(handle, "glBindTexture")`
 *     resolves GLES symbols through the dependency chain.
 */

#include <stddef.h>

/* GLX stubs -- probes that return NULL mean "no GLX context, use EGL". */
void *glXGetCurrentContext(void)             { return NULL; }
void *glXGetCurrentDisplay(void)             { return NULL; }
void *glXGetProcAddress(const char *name)    { (void)name; return NULL; }
void *glXGetProcAddressARB(const char *name) { (void)name; return NULL; }
