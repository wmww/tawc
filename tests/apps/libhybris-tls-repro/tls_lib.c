// Bionic-built shared library with a __thread variable. Loaded by
// repro.c via hybris_dlopen + hybris_dlclose to drive the TLS-module
// register/unregister path in libhybris's vendored bionic linker.
//
// The presence of `__thread g_tls_var` is the entire point: it makes
// the .so participate in TLS-module bookkeeping, so dlclose hits
// unregister_soinfo_tls() — which is where the historical
// `linker_tls.cpp:93 CHECK 'mod.static_offset == SIZE_MAX' failed`
// abort would fire on libhybris before the lazy-promote-to-static fix.
//
// Cross-built with the Android NDK (target aarch64-linux-android) by
// scripts/install-test-deps.sh. Plain dlopen would reject this on
// glibc; libhybris is what makes it loadable.
#include <stdio.h>

__thread int g_tls_var = 42;
__thread int g_tls_zero;
__thread char g_tls_pad[32];
/* Uninitialised TLS object exercising a meaningful .tbss tail. memsz
 * here is 256 bytes past filesz, so a regression that bounds-checks
 * against filesz only (init_size) instead of memsz (segment.size) lets
 * a write to g_tbss_block[255] land in adjacent IE TLS without
 * tripping. */
__thread char g_tbss_block[256];


int get_tls(void) { return g_tls_var; }
void set_tls(int v) { g_tls_var = v; }
int get_zero_tls(void) { return g_tls_zero; }
void set_zero_tls(int v) { g_tls_zero = v; }
int get_pad_sum(void) {
    int sum = 0;
    for (size_t i = 0; i < sizeof(g_tls_pad); i++) {
        sum += g_tls_pad[i];
    }
    return sum;
}
void set_pad_first(int v) { g_tls_pad[0] = (char)v; }
int get_tbss_sum(void) {
    int sum = 0;
    for (size_t i = 0; i < sizeof(g_tbss_block); i++) {
        sum += (unsigned char)g_tbss_block[i];
    }
    return sum;
}
int get_tbss_high(void) { return (unsigned char)g_tbss_block[sizeof(g_tbss_block) - 1]; }
void set_tbss_high(int v) { g_tbss_block[sizeof(g_tbss_block) - 1] = (char)v; }

__attribute__((constructor))
static void on_load(void) {
    fprintf(stderr, "tls_lib: ctor, &g_tls_var=%p\n", (void*)&g_tls_var);
}

__attribute__((destructor))
static void on_unload(void) {
    fprintf(stderr, "tls_lib: dtor\n");
}
