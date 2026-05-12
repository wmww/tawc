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

/// In-rootfs graphics backend pick. Mirrors `me.phie.tawc.GraphicsBackend`
/// (Kotlin). Tests pass a value to the spawn helpers
/// ([`crate::rootfs_process::RootfsProcess::spawn_with`],
/// [`crate::adb::rootfs_run_with`], `helpers::launch_and_wait_for_*`,
/// `helpers::assert_renders_via_*`) to run that one client under a
/// specific backend without touching the user's persisted Settings
/// pick. The override travels over the broker as a `GRAPHICS <key>`
/// header on the RUNINSIDE form (see `notes/exec-broker.md`); the
/// backend-less call shapes (`spawn`, `rootfs_run`) honour the user's
/// UI pick.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GraphicsBackend {
    Libhybris,
    Gfxstream,
    Cpu,
    LibhybrisZink,
}

impl GraphicsBackend {
    pub fn as_key(&self) -> &'static str {
        match self {
            GraphicsBackend::Libhybris => "libhybris",
            GraphicsBackend::Gfxstream => "gfxstream",
            GraphicsBackend::Cpu => "cpu",
            GraphicsBackend::LibhybrisZink => "libhybris-zink",
        }
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
