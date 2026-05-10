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
//     We try to force the slow path the way the simple repro doesn't:
//     repeatedly dlopen a TLS-using .so, recycling slots by dlclosing
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
//  4. Per-thread isolation and replay. A promoted module's .tdata must
//     be applied to every thread, writes on one thread must not bleed
//     into another, and the replay registry must remain valid after the
//     original .so has been dlclose'd.
//
// Pass: clean exit 0 with all asserts surviving and final get_tls
// returning 42.
// Fail: stderr line indicating which assert tripped, then exit 1
// (assertion-style) or exit 134 (libhybris CHECK abort still wins).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

extern void* hybris_dlopen(const char* filename, int flag);
extern void* hybris_dlsym(void* handle, const char* symbol);
extern int   hybris_dlclose(void* handle);
extern char* hybris_dlerror(void);
extern void* _hybris_hook___get_tls_hooks(void);

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

typedef int (*get_int_fn)(void);
typedef void (*set_int_fn)(int);

struct tls_api {
    get_int_fn get_tls;
    set_int_fn set_tls;
    get_int_fn get_zero_tls;
    set_int_fn set_zero_tls;
    get_int_fn get_pad_sum;
    set_int_fn set_pad_first;
};

static int expect_int(const char* what, int got, int expected) {
    fprintf(stderr, "[repro] %s = %d (expected %d)\n", what, got, expected);
    if (got != expected) {
        fprintf(stderr, "[repro] FAIL: %s mismatch\n", what);
        return 1;
    }
    return 0;
}

static int resolve_api(void* h, struct tls_api* api, const char* label) {
    api->get_tls = (get_int_fn)hybris_dlsym(h, "get_tls");
    api->set_tls = (set_int_fn)hybris_dlsym(h, "set_tls");
    api->get_zero_tls = (get_int_fn)hybris_dlsym(h, "get_zero_tls");
    api->set_zero_tls = (set_int_fn)hybris_dlsym(h, "set_zero_tls");
    api->get_pad_sum = (get_int_fn)hybris_dlsym(h, "get_pad_sum");
    api->set_pad_first = (set_int_fn)hybris_dlsym(h, "set_pad_first");

    if (!api->get_tls || !api->set_tls || !api->get_zero_tls ||
        !api->set_zero_tls || !api->get_pad_sum || !api->set_pad_first) {
        fprintf(stderr, "[repro] %s: hybris_dlsym failed: %s\n", label, hybris_dlerror());
        return 1;
    }
    return 0;
}

static int check_initial_tls(const struct tls_api* api, const char* label) {
    if (expect_int(label, api->get_tls(), EXPECTED_TLS_VALUE)) return 1;
    if (expect_int("get_zero_tls()", api->get_zero_tls(), 0)) return 1;
    if (expect_int("get_pad_sum()", api->get_pad_sum(), 0)) return 1;
    return 0;
}

struct thread_check_args {
    struct tls_api api;
    int initial_value;
    int write_value;
    int zero_write_value;
    int pad_write_value;
};

static void* tls_thread_check(void* opaque) {
    struct thread_check_args* args = (struct thread_check_args*)opaque;

    // Force per-thread bionic TLS setup and registry catch-up before
    // calling into the bionic .so. This is the documented entry path for
    // glibc-created threads that later touch libhybris-mapped code.
    _hybris_hook___get_tls_hooks();

    if (expect_int("thread get_tls()", args->api.get_tls(), args->initial_value)) {
        return (void*)(intptr_t)1;
    }
    if (expect_int("thread get_zero_tls()", args->api.get_zero_tls(), 0)) {
        return (void*)(intptr_t)1;
    }
    if (expect_int("thread get_pad_sum()", args->api.get_pad_sum(), 0)) {
        return (void*)(intptr_t)1;
    }

    args->api.set_tls(args->write_value);
    args->api.set_zero_tls(args->zero_write_value);
    args->api.set_pad_first(args->pad_write_value);

    if (expect_int("thread get_tls() after set", args->api.get_tls(), args->write_value)) {
        return (void*)(intptr_t)1;
    }
    if (expect_int("thread get_zero_tls() after set",
                   args->api.get_zero_tls(), args->zero_write_value)) {
        return (void*)(intptr_t)1;
    }
    if (expect_int("thread get_pad_sum() after set",
                   args->api.get_pad_sum(), args->pad_write_value)) {
        return (void*)(intptr_t)1;
    }
    return NULL;
}

static int run_thread_isolation_check(const struct tls_api* api) {
    fprintf(stderr, "[repro] thread isolation check...\n");
    api->set_tls(7);
    api->set_zero_tls(11);
    api->set_pad_first(13);

    struct thread_check_args args = {
        .api = *api,
        .initial_value = EXPECTED_TLS_VALUE,
        .write_value = 99,
        .zero_write_value = 123,
        .pad_write_value = 17,
    };

    pthread_t thread;
    if (pthread_create(&thread, NULL, tls_thread_check, &args) != 0) {
        fprintf(stderr, "[repro] FAIL: pthread_create failed\n");
        return 1;
    }

    void* thread_result = NULL;
    if (pthread_join(thread, &thread_result) != 0) {
        fprintf(stderr, "[repro] FAIL: pthread_join failed\n");
        return 1;
    }
    if (thread_result != NULL) {
        fprintf(stderr, "[repro] FAIL: thread TLS check failed\n");
        return 1;
    }

    if (expect_int("main get_tls() after child set", api->get_tls(), 7)) return 1;
    if (expect_int("main get_zero_tls() after child set", api->get_zero_tls(), 11)) return 1;
    if (expect_int("main get_pad_sum() after child set", api->get_pad_sum(), 13)) return 1;
    fprintf(stderr, "[repro] thread isolation check OK\n");
    return 0;
}

static void* replay_after_dlclose_thread(void* opaque) {
    (void)opaque;
    fprintf(stderr, "[repro] post-dlclose replay thread: _hybris_hook___get_tls_hooks()\n");
    _hybris_hook___get_tls_hooks();
    fprintf(stderr, "[repro] post-dlclose replay thread: replay OK\n");
    return NULL;
}

static int run_post_dlclose_replay_check(void) {
    fprintf(stderr, "[repro] post-dlclose replay check...\n");
    pthread_t thread;
    if (pthread_create(&thread, NULL, replay_after_dlclose_thread, NULL) != 0) {
        fprintf(stderr, "[repro] FAIL: pthread_create for replay check failed\n");
        return 1;
    }
    void* thread_result = NULL;
    if (pthread_join(thread, &thread_result) != 0) {
        fprintf(stderr, "[repro] FAIL: pthread_join for replay check failed\n");
        return 1;
    }
    if (thread_result != NULL) {
        fprintf(stderr, "[repro] FAIL: post-dlclose replay thread failed\n");
        return 1;
    }
    fprintf(stderr, "[repro] post-dlclose replay check OK\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-bionic-tls-lib.so>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    // ---- Failure modes #3 + #4: value correctness, .tbss zeroing,
    // and per-thread isolation while the library is still loaded.
    fprintf(stderr, "[repro] initial hybris_dlopen(%s)\n", path);
    void* initial = hybris_dlopen(path, 2 /* RTLD_NOW */);
    if (!initial) {
        fprintf(stderr, "[repro] initial hybris_dlopen failed: %s\n", hybris_dlerror());
        return 1;
    }
    fprintf(stderr, "[repro] initial handle=%p\n", initial);

    struct tls_api initial_api;
    if (resolve_api(initial, &initial_api, "initial")) return 1;
    if (check_initial_tls(&initial_api, "get_tls()")) return 1;
    if (run_thread_isolation_check(&initial_api)) return 1;

    fprintf(stderr, "[repro] initial hybris_dlclose...\n");
    int initial_close = hybris_dlclose(initial);
    fprintf(stderr, "[repro] initial hybris_dlclose -> %d\n", initial_close);
    if (initial_close != 0) {
        fprintf(stderr, "[repro] FAIL: initial hybris_dlclose returned non-zero\n");
        return 1;
    }

    // The new thread has never touched libhybris TLS. If the promoted-
    // TLS registry kept a raw .tdata pointer into the now-unloaded .so,
    // this catch-up replay can read unmapped memory and crash.
    if (run_post_dlclose_replay_check()) return 1;

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
        struct tls_api api;
        if (resolve_api(h, &api, "stress")) return 1;
        // The TLS access itself is what trips the dynamic-resolver bug
        // -- fetching a value, not opening, is what runs through the
        // patched MRS + TLSDESC resolver. Also assert the initialiser so
        // every iteration checks both TLSDESC and .tdata replay.
        if (expect_int("stress get_tls()", api.get_tls(), EXPECTED_TLS_VALUE)) {
            return 1;
        }
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

    struct tls_api api;
    if (resolve_api(h, &api, "final")) return 1;
    int v = api.get_tls();
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
    if (r != 0) {
        fprintf(stderr, "[repro] FAIL: final hybris_dlclose returned non-zero\n");
        return 1;
    }
    fprintf(stderr, "[repro] survived; no abort, value correct\n");
    return 0;
}
