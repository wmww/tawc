//! libhybris regression tests. Bug-for-bug repros of historical aborts
//! that don't fit cleanly into apps:: or graphics:: — those drive real
//! desktop/GPU stacks and only catch libhybris bugs incidentally; these
//! drive the suspect libhybris call directly.
//!
//! libhybris is aarch64-only in tawc, so these fail on the emulator.

use tawc_integration::{adb, rootfs};

/// Regression test for libhybris's TLS handling. Three historical
/// regressions get covered in one round-trip; see repro.c for the
/// per-failure-mode breakdown. Quickly:
///
/// 1. unregister_tls_module CHECK abort on dlclose (Pixel Fold lxterminal).
///    Pre-promote-fix every TLS-using dlopen reserved a static slot
///    that unregister refused to free.
/// 2. TLSDESC dynamic resolver routed to glibc's __tls_get_addr
///    (Pixel 4a GTK/firefox; OnePlus 9 silent garbage). Post-promote-fix
///    register_soinfo_tls always registered dynamic, but TLSDESC's
///    handler kept the dynamic-resolver branch — its slow path calls
///    __tls_get_addr against host glibc with a bionic module_id, so
///    glibc's _dl_update_slotinfo asserts. Fixed by also promoting on
///    R_GENERIC_TLSDESC, mirroring the IE path.
/// 3. Per-.so __thread initial values silently zeroed (every device).
///    The patcher redirected bionic TPIDR reads to a fixed shared
///    tls_area.slots[] with no .tdata init, so a `__thread int g = 42`
///    in a dlopened bionic .so came back as 0. Fixed by reserving
///    bionic_tcb at the head of static_tls_layout, setting tls_tp_base
///    to offset_thread_pointer() (=16 on aarch64), and calling back
///    from promote_tls_module_to_static into hooks.c to memcpy .tdata
///    into the calling thread's tls_static_tls.
/// 4. Per-thread isolation and post-dlclose replay. A child thread must
///    see fresh initial TLS values, writes must not leak between threads,
///    and a new thread must be able to replay promoted-TLS initializers
///    after the original .so has been dlclose'd.
///
/// Repro binary at tests/apps/libhybris-tls-repro/. The bionic-side
/// tls_lib.so (a tiny .so with `__thread int g_tls_var = 42;`) is
/// NDK-cross-built on the host by scripts/install-test-deps.sh; the
/// glibc-side `repro` executable is built inside the rootfs and links
/// `-lhybris-common`. Drives a full hybris_dlopen + hybris_dlsym +
/// hybris_dlclose round-trip plus thread isolation, post-dlclose replay,
/// a stress loop, and an assert that get_tls() returns the declared
/// initialiser.
///
/// Failure modes:
///   - SIGABRT (exit 134) inside hybris_dlclose — regression of #1
///   - SIGABRT (exit 134) inside the stress loop reading
///     `Inconsistency detected by ld.so: dl-tls.c: 857:
///     _dl_update_slotinfo: Assertion 'max_modid >= req_modid' failed!`
///     — regression of #2
///   - exit 1 with `get_tls() = 0 (expected 42)` and a libhybris
///     pointer to the broken code path — regression of #3
///   - exit 1 or SIGSEGV around `post-dlclose replay check` — the
///     promoted-TLS registry kept a pointer into an unloaded .so instead
///     of owning the initializer bytes
#[test]
fn test_libhybris_tls_dlclose_does_not_abort() {
    let bin = rootfs::ensure_libhybris_tls_repro().expect("libhybris-tls-repro build");

    // Run from inside the install dir so the relative `./tls_lib.so`
    // arg keeps the test self-contained.
    let cmd = format!("cd /tmp/libhybris-tls-repro && {} ./tls_lib.so", bin);
    let output = adb::rootfs_run(&cmd).expect("run libhybris-tls-repro");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    // Exit code maps to the failure mode (see fn doc + repro.c):
    //   134 = SIGABRT, either the unregister CHECK or the
    //         glibc _dl_update_slotinfo assertion;
    //     1 = repro's own assert (e.g. get_tls() != 42, or a hybris_dlopen
    //         failure mid-stress).
    // Any non-zero is a libhybris regression -- surface enough hints in
    // the message that someone seeing this in CI knows where to look.
    assert!(
        output.status.success(),
        "libhybris-tls-repro exited non-zero ({:?}).\n\
         - exit 134 + `unregister_tls_module CHECK 'mod.static_offset == SIZE_MAX' failed`\n\
           => the IE/TLSDESC lazy-promote fix in libhybris linker_tls.cpp has regressed\n\
         - exit 134 + `Inconsistency detected by ld.so: dl-tls.c: ... _dl_update_slotinfo`\n\
           => TLSDESC promote has regressed -- the dynamic resolver is now reaching\n\
              glibc __tls_get_addr with a bionic module_id (see linker.cpp\n\
              R_GENERIC_TLSDESC handler)\n\
         - exit 1 + `get_tls() = 0 (expected 42)`\n\
           => promote_tls_module_to_static is not pushing .tdata into the calling\n\
              thread's tls_static_tls, OR the bionic_tcb / tls_tp_base math has\n\
              drifted (see hooks.c::tls_static_tls + linker_tls.cpp + linker.cpp\n\
              tls_tp_base)\n\
         stdout: {stdout}\nstderr: {stderr}",
        output.status.code()
    );

    // Belt-and-braces: the repro prints these on a successful round-trip.
    // Catches the (unlikely) case where it exits 0 having taken a different
    // failure path entirely (e.g. dlopen returning a stub).
    assert!(
        stderr.contains("hybris_dlclose -> 0"),
        "libhybris-tls-repro exited 0 but never printed `hybris_dlclose -> 0` -- \
         did dlopen actually succeed?\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("thread isolation check OK"),
        "libhybris-tls-repro exited 0 but never completed the per-thread TLS \
         isolation check.\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("post-dlclose replay check OK"),
        "libhybris-tls-repro exited 0 but never completed the post-dlclose \
         promoted-TLS replay check.\nstdout: {stdout}\nstderr: {stderr}"
    );
    assert!(
        stderr.contains("survived; no abort, value correct"),
        "libhybris-tls-repro exited 0 but never reached the final post-dlclose \
         line -- did execution end before the value-correctness assert ran?\n\
         stdout: {stdout}\nstderr: {stderr}"
    );
}
