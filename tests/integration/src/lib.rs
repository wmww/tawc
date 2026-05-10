pub mod adb;
pub mod compositor;
pub mod debug_app;
pub mod helpers;
pub mod rootfs;
pub mod rootfs_process;

use std::sync::OnceLock;

/// On-device scratch dir for everything that doesn't live in the app's
/// private data dir. Test/debug only — production never touches this.
/// See `scripts/lib/tawc-scratch.sh` for the rationale.
pub const TAWC_SCRATCH: &str = "/data/local/tmp/tawc-dev";

/// Install id for the in-app distro this test run targets. Used to
/// parameterize the `/data/data/.../distros/<id>/` paths so a single
/// APK can host multiple installs (e.g. an `arch` chroot alongside an
/// `arch-tawcroot`) and the suite can target either.
///
/// Resolution mirrors `scripts/lib/tawc-install-id.sh`: honour
/// `TAWC_INSTALL_ID` if set, otherwise enumerate
/// `distros/*/metadata.json` on-device and use the unique match.
/// Panics if there is no install, or more than one and no env-var pin —
/// the suite refuses to guess rather than silently target the wrong slot.
/// Cached after the first call.
pub fn install_id() -> String {
    static ID: OnceLock<String> = OnceLock::new();
    ID.get_or_init(resolve_install_id).clone()
}

/// In-app graphics-driver pick the test runner flipped before the
/// suite started. Mirrors `Settings.graphicsBackend` (Kotlin); `"libhybris"`
/// is the assumed default when the runner didn't pass `--graphics
/// gfxstream`. Read by tests that gate libhybris-specific assertions
/// (Android META-EGL, vkcube WSI, …) — those have no working analogue
/// under the gfxstream-bridge backend until phases 4–5 land
/// (notes/gfxstream-bridge.md "Remaining work to a fully-integrated
/// bridge backend"). Tests that share a code path between backends
/// don't need to consult this.
pub fn graphics_backend() -> String {
    std::env::var("TAWC_GRAPHICS_BACKEND")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "libhybris".to_string())
}

/// Skip-guard for tests that fundamentally exercise a libhybris-only
/// path (the chroot's `LD_LIBRARY_PATH` points at libhybris, EGL-via-
/// libhybris is the only working GL driver, etc.). Returns `true` when
/// the test should bail out cleanly under `--graphics gfxstream`; the
/// caller prints a `SKIP:` line explaining the gap and `return`s.
///
/// `reason` should be a short human-readable explanation that points at
/// the relevant phase in `notes/gfxstream-bridge.md` so a future reader
/// knows what would unblock the test. Examples:
///   - `"GL via Zink-on-bridge (phase 6) not implemented yet"`
///   - `"Vulkan WSI (phases 4-5) not implemented yet"`
///   - `"libhybris-only — no analogue under bridge"`
pub fn skip_if_gfxstream(reason: &str) -> bool {
    if graphics_backend() == "gfxstream" {
        eprintln!("SKIP: {reason}");
        true
    } else {
        false
    }
}

fn resolve_install_id() -> String {
    if let Ok(v) = std::env::var("TAWC_INSTALL_ID") {
        if !v.is_empty() {
            return v;
        }
    }
    let pkg = "me.phie.tawc";
    let probe = format!(
        "for d in /data/data/{pkg}/distros/*/metadata.json; \
         do test -f \"$d\" && basename \"$(dirname \"$d\")\"; done"
    );
    // Run via the dev exec broker (runs as the app uid). No root, no
    // run-as. Same code path the host scripts use; see
    // `notes/exec-broker.md`.
    let output = crate::adb::rootfs_host_exec(&["/system/bin/sh", "-c", &probe])
        .expect("failed to invoke broker to resolve TAWC_INSTALL_ID");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut ids: Vec<&str> = stdout
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect();
    ids.sort_unstable();
    ids.dedup();
    match ids.as_slice() {
        [one] => (*one).to_string(),
        [] => panic!(
            "no in-app install found at /data/data/{pkg}/distros/*/ — \
             install one with `bash scripts/install-distro.sh <id> [method]` \
             (see CLAUDE.md Quick Reference)"
        ),
        many => panic!(
            "multiple installs found at /data/data/{pkg}/distros/ ({}); \
             pick one with TAWC_INSTALL_ID=<id>",
            many.join(", ")
        ),
    }
}
