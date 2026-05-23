//! Wraps the cleat-driven tawcroot device test suite as a single
//! integration-test case. The cleat orchestrator (built by
//! `tawcroot/build.sh --abi=<abi> --testhost --tests`) runs all four
//! tawcroot test layers (unit / handler / integration / future diff)
//! on-device as the adb shell uid, so plumbing the entire suite through one
//! `#[test]` keeps `run-integration-tests.sh` as the single command
//! that exercises every test we have.
//!
//! Output is captured rather than streamed: cleat prints one
//! progress line per test and tawcroot has hundreds of cases, so
//! interleaving that into libtest's `--nocapture` stream would drown
//! out the rest of the integration run. On failure the test panics
//! with the captured stdout/stderr appended so the operator sees the
//! cleat report without rerunning anything.

use std::path::PathBuf;
use std::process::Command;

#[test]
fn test_tawcroot_device_suite() {
    tawc_integration::helpers::test_init();
    let repo_root: PathBuf = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    // `--device` invokes `scripts/lib/select-device.sh`, which short-circuits
    // when ANDROID_SERIAL is already set — `run-integration-tests.sh`
    // has already selected and exported it, so we inherit that here.
    // `--no-build` skips the cross-compile: run-integration-tests.sh is
    // expected to have built `tawcroot/build.sh --abi=<abi> --testhost --tests`
    // and the fixtures already.
    let output = Command::new("tawcroot/test.sh")
        .arg("--device")
        .arg("--no-build")
        .current_dir(&repo_root)
        .output()
        .expect("failed to spawn `tawcroot/test.sh --device --no-build`");

    if !output.status.success() {
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        panic!(
            "tawcroot device suite failed (exit {:?})\n\
             ----- stdout -----\n{}\n\
             ----- stderr -----\n{}",
            output.status.code(),
            stdout,
            stderr,
        );
    }
}
