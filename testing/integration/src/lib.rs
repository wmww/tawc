pub mod adb;
pub mod chroot;
pub mod chroot_process;
pub mod compositor;
pub mod debug_app;
pub mod helpers;

use std::sync::OnceLock;

/// On-device scratch dir for everything that doesn't live in the app's
/// private data dir. Test/debug only — production never touches this.
/// See `client/tawc-scratch.sh` for the rationale.
pub const TAWC_SCRATCH: &str = "/data/local/tmp/tawc-dev";

/// Install id for the in-app distro this test run targets. Used to
/// parameterize the `/data/data/.../distros/<id>/` paths so a single
/// APK can host multiple installs (e.g. an `arch` chroot alongside an
/// `arch-tawcroot`) and the suite can target either.
///
/// Resolution mirrors `client/tawc-install-id.sh`: honour
/// `TAWC_INSTALL_ID` if set, otherwise enumerate
/// `distros/*/metadata.json` on-device and use the unique match.
/// Panics if there is no install, or more than one and no env-var pin —
/// the suite refuses to guess rather than silently target the wrong slot.
/// Cached after the first call.
pub fn install_id() -> String {
    static ID: OnceLock<String> = OnceLock::new();
    ID.get_or_init(resolve_install_id).clone()
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
    let cmd = format!(
        "run-as {pkg} sh -c '{probe}' 2>/dev/null; su -c '{probe}' 2>/dev/null"
    );
    let output = std::process::Command::new("adb")
        .args(["shell", &cmd])
        .output()
        .expect("failed to invoke `adb shell` to resolve TAWC_INSTALL_ID");
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
             install one with `adb shell am start -n {pkg}/.install.InstallActivity \
             --es autoStart true --es id <id>` (see CLAUDE.md Quick Reference)"
        ),
        many => panic!(
            "multiple installs found at /data/data/{pkg}/distros/ ({}); \
             pick one with TAWC_INSTALL_ID=<id>",
            many.join(", ")
        ),
    }
}
