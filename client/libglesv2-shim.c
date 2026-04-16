/**
 * libGLESv2.so.2 shim -- GLX stubs + GLES forwarding via DT_NEEDED.
 *
 * Mirrors libgl-shim.c. GTK3's libepoxy dlsyms the open handle of every GL
 * library it finds (including libGLESv2) for a GLX symbol; on a GLES-only
 * system with no stub, the probe can land on a real-but-broken GLX vendor
 * and libepoxy will `abort()` instead of falling back to EGL. Exporting
 * NULL-returning stubs from the libGLESv2 path that libepoxy opens keeps
 * it in the "use EGL" branch.
 */

#include <stddef.h>

void *glXGetCurrentContext(void)             { return NULL; }
void *glXGetCurrentDisplay(void)             { return NULL; }
void *glXGetProcAddress(const char *name)    { (void)name; return NULL; }
void *glXGetProcAddressARB(const char *name) { (void)name; return NULL; }
