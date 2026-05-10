// Regression tests for libhybris's TLS path. Each path used to break in
// a different way before our fork's bionic_tcb-reserve + IE/TLSDESC
// promote-to-static + per-thread .tdata-init changes; the asserts here
// are constructed to fail loudly on either historical regression as well
// as a few new ones a future "let's recycle dynamic slots" rewrite is
// likely to reintroduce.
//
// Failure modes the asserts cover:
//
//  1. unregister_tls_module CHECK abort on dlclose  (pre-promote-fix).
//     Triggered by dlopen+dlclose of any TLS-using bionic .so. If
//     libhybris regresses to "all loads reserve static slot
//     unconditionally", the CHECK at linker_tls.cpp:93 aborts in
//     hybris_dlclose with exit 134.
//
//  2. TLSDESC dynamic-resolver path silently routed to glibc's
//     __tls_get_addr  (post-promote-fix, Pixel-4a-only failure mode).
//     With "register dynamic, lazy-promote on IE only", every TLSDESC
//     access to a dlopened .so's __thread var routes through
//     tlsdesc_resolver_dynamic_slow_path -> __tls_get_addr. That symbol
//     resolves against host glibc (libhybris_common.so links against
//     libc), so glibc's _dl_update_slotinfo asserts on a bionic
//     module_id that has no entry in glibc's slotinfo table. Manifests
//     loudly on devices whose bionic stack churns the TLS-modules
//     generation enough to flip the cmp in tlsdesc_resolver_dynamic
//     (Pixel 4a's Adreno + GTK init does; OnePlus 9 + the simple
//     repro doesn't, which is why the OnePlus tests passed and the
//     Pixel ones blew up).
//
//     We force the slow path the way the simple repro doesn't: dlopen
//     a single TLS-using .so 200 times, recycling slots by dlclosing
//     between iterations. Each register/unregister bumps
//     __libc_shared_globals()->tls_modules.generation; eventually it
//     overtakes the host glibc DTV gen the resolver compares against,
//     `cmp x21, x22; b.lo .fallback` branches, and the slow path runs
//     glibc's __tls_get_addr -> Inconsistency detected by ld.so abort.
//
//  3. Per-.so __thread storage broken for ALL devices  (existed since
//     the patcher landed; surfaced as silent garbage). bionic .so
//     `__thread int g_var = 42;` returned 0 because the patcher
//     redirected reads to a fixed shared tls_area.slots[] with no
//     .tdata init. Asserting `get_tls() == 42` catches both a future
//     regression of the per-thread-.tdata-copy and any drift in the
//     bionic_tcb reservation that shifts static_offset off the patched
//     access.
//
// Pass: clean exit 0 with all asserts surviving and the second-handle
// get_tls returning 42.
// Fail: stderr line indicating which assert tripped, then exit 1
// (assertion-style) or exit 134 (libhybris CHECK abort still wins).
#include <stdio.h>

extern void* hybris_dlopen(const char* filename, int flag);
extern void* hybris_dlsym(void* handle, const char* symbol);
extern int   hybris_dlclose(void* handle);
extern char* hybris_dlerror(void);

#define EXPECTED_TLS_VALUE 42
// Each register/unregister cycle bumps libhybris's bionic
// __libc_shared_globals.tls_modules.generation by 1, and also leaks
// ~56 bytes of libhybris's static-TLS layout (the bionic linker
// reloads tls_lib's transitive deps + apex libc bookkeeping per
// dlopen, all of which promote; promoted slots can't be recycled
// without risking a static-pointer free in the dynamic-tls path --
// see linker_tls.cpp::get_unused_module_index). With
// hooks.c::BIONIC_STATIC_TLS_SIZE = 1024, after bionic_tcb (80B) and
// first-iteration libc baseline (~80B), the headroom is ~864B / 56B/iter
// = ~15 iters. 10 iters leaves headroom for variation across devices
// while still bumping bionic_gen past ~22 (well over the typical
// glibc gen of a libhybris-loaded process, ~10).
//
// Note this is a "regression catcher", not a complete reproducer: with
// the dynamic-resolver bug present, this loop will sometimes survive
// without aborting if the local glibc gen happened to land above ~22
// (varies per glibc build / per process). The post-loop value-correctness
// assert (get_tls() == 42) is what catches the bug deterministically;
// the stress is just opportunistic extra coverage for the abort path.
#define STRESS_ITERATIONS 10

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-bionic-tls-lib.so>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    // ---- Failure mode #2: stress register/unregister to force the
    // TLSDESC dynamic-resolver fallback path. If the static-promote on
    // TLSDESC has regressed back to dynamic-only, this loop will trip
    // glibc's _dl_update_slotinfo assertion before iteration STRESS_ITERATIONS.
    fprintf(stderr, "[repro] stress: %d x (hybris_dlopen + get_tls + hybris_dlclose)\n",
            STRESS_ITERATIONS);
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        void* h = hybris_dlopen(path, 2 /* RTLD_NOW */);
        if (!h) {
            fprintf(stderr, "[repro] stress: hybris_dlopen failed at iter %d: %s\n",
                    i, hybris_dlerror());
            return 1;
        }
        int (*get_tls)(void) = (int(*)(void))hybris_dlsym(h, "get_tls");
        if (!get_tls) {
            fprintf(stderr, "[repro] stress: hybris_dlsym(get_tls) failed at iter %d: %s\n",
                    i, hybris_dlerror());
            return 1;
        }
        // The TLS access itself is what trips the dynamic-resolver bug
        // -- fetching a value, not opening, is what runs through the
        // patched MRS + TLSDESC resolver. Discard the result here; the
        // value-correctness check happens in the post-loop dlopen.
        (void)get_tls();
        if (hybris_dlclose(h) != 0) {
            fprintf(stderr, "[repro] stress: hybris_dlclose failed at iter %d\n", i);
            return 1;
        }
    }
    fprintf(stderr, "[repro] stress: %d iterations OK (no fallback->__tls_get_addr abort)\n",
            STRESS_ITERATIONS);

    // ---- Failure mode #1 + #3: dlopen one more time, read the TLS
    // variable, and require it return its declared initialiser. Catches
    // both the pre-existing "patcher reads garbage from shared
    // tls_area.slots" regression and any future drift in the
    // bionic_tcb-reserve / static_offset math. Final dlclose validates
    // the unregister path doesn't abort.
    fprintf(stderr, "[repro] hybris_dlopen(%s)\n", path);
    void* h = hybris_dlopen(path, 2 /* RTLD_NOW */);
    if (!h) {
        fprintf(stderr, "[repro] hybris_dlopen failed: %s\n", hybris_dlerror());
        return 1;
    }
    fprintf(stderr, "[repro] handle=%p\n", h);

    int (*get_tls)(void) = (int(*)(void))hybris_dlsym(h, "get_tls");
    if (!get_tls) {
        fprintf(stderr, "[repro] hybris_dlsym(get_tls) failed: %s\n", hybris_dlerror());
        return 1;
    }
    int v = get_tls();
    fprintf(stderr, "[repro] get_tls() = %d (expected %d)\n", v, EXPECTED_TLS_VALUE);
    if (v != EXPECTED_TLS_VALUE) {
        fprintf(stderr, "[repro] FAIL: __thread initialiser not honoured by libhybris\n");
        fprintf(stderr, "[repro]   This means promote_tls_module_to_static() is not\n");
        fprintf(stderr, "[repro]   copying .tdata into the calling thread's bionic\n");
        fprintf(stderr, "[repro]   static-TLS area, OR the patcher offset / bionic_tcb\n");
        fprintf(stderr, "[repro]   reservation no longer line up. See hooks.c\n");
        fprintf(stderr, "[repro]   tls_static_tls + linker_tls.cpp::promote_tls_module_to_static.\n");
        return 1;
    }

    fprintf(stderr, "[repro] hybris_dlclose...\n");
    int r = hybris_dlclose(h);
    fprintf(stderr, "[repro] hybris_dlclose -> %d\n", r);
    fprintf(stderr, "[repro] survived; no abort, value correct\n");
    return 0;
}
