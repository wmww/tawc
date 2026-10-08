//! Launcher entry-management coverage via the debug `launcher-list` /
//! `set-entry-override` / `put-shortcut` broker actions (notes/launcher.md,
//! notes/exec-broker.md). No screenshot scraping: plant a `.desktop` file
//! in the rootfs, write the launcher store through the same locked write
//! the UI performs, and assert on the merged list the launcher renders
//! from.

use tawc_integration::adb;
use tawc_integration::helpers::assert_broker_ok;

const ENTRY_ID: &str = "tawc-hide-test";

fn rootfs() -> String {
    format!(
        "{}/distros/{}/rootfs",
        tawc_integration::app_data_dir(),
        tawc_integration::install_id()
    )
}

fn desktop_path() -> String {
    format!("{}/usr/share/applications/{ENTRY_ID}.desktop", rootfs())
}

/// Best-effort cleanup on both success and panic: unhide the id (the
/// metadata write is durable) and remove the planted `.desktop`.
struct Cleanup;

impl Drop for Cleanup {
    fn drop(&mut self) {
        let _ = adb::set_entry_hidden(ENTRY_ID, false);
        let rm = format!("rm -f '{}'", desktop_path());
        let _ = adb::rootfs_host_exec(&["/system/bin/sh", "-c", &rm]);
    }
}

/// The top-level objects of a JSON array, as text. String-aware brace
/// matching, since entries nest (`packaged`, `env`) and the crate has no
/// JSON dep.
fn objects(list: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let (mut depth, mut start, mut in_str, mut esc) = (0, 0, false, false);
    for (i, c) in list.char_indices() {
        if in_str {
            match c {
                _ if esc => esc = false,
                '\\' => esc = true,
                '"' => in_str = false,
                _ => {}
            }
            continue;
        }
        match c {
            '"' => in_str = true,
            '{' => {
                if depth == 0 {
                    start = i;
                }
                depth += 1;
            }
            '}' => {
                depth -= 1;
                if depth == 0 {
                    out.push(&list[start..=i]);
                }
            }
            _ => {}
        }
    }
    out
}

/// Raw JSON text of `key`'s value at the top level of `obj`.
fn raw_field<'a>(obj: &'a str, key: &str) -> Option<&'a str> {
    let needle = format!("\"{key}\":");
    let (mut depth, mut in_str, mut esc) = (0, false, false);
    let bytes = obj.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i] as char;
        if in_str {
            match c {
                _ if esc => esc = false,
                '\\' => esc = true,
                '"' => in_str = false,
                _ => {}
            }
            i += 1;
            continue;
        }
        if depth == 1 && obj[i..].starts_with(&needle) {
            let rest = &obj[i + needle.len()..];
            let end = value_end(rest);
            return Some(&rest[..end]);
        }
        match c {
            '"' => in_str = true,
            '{' | '[' => depth += 1,
            '}' | ']' => depth -= 1,
            _ => {}
        }
        i += 1;
    }
    None
}

/// Length of the JSON value at the start of `s`.
fn value_end(s: &str) -> usize {
    let (mut depth, mut in_str, mut esc) = (0, false, false);
    for (i, c) in s.char_indices() {
        if in_str {
            match c {
                _ if esc => esc = false,
                '\\' => esc = true,
                '"' => {
                    in_str = false;
                    if depth == 0 {
                        return i + 1;
                    }
                }
                _ => {}
            }
            continue;
        }
        match c {
            '"' => in_str = true,
            '{' | '[' => depth += 1,
            '}' | ']' => {
                if depth == 0 {
                    return i;
                }
                depth -= 1;
                if depth == 0 {
                    return i + 1;
                }
            }
            ',' if depth == 0 => return i,
            _ => {}
        }
    }
    s.len()
}

/// The entry object with this id in a `launcher-list` array.
fn entry_object<'a>(list: &'a str, id: &str) -> Option<&'a str> {
    let want = format!("\"{id}\"");
    objects(list).into_iter().find(|o| raw_field(o, "id") == Some(want.as_str()))
}

/// Hide/unhide round-trip: a hidden entry disappears from the default
/// (launcher-visible) list, stays reachable with `showHidden` and is
/// flagged `hidden:true` there, and returns on unhide.
#[test]
fn test_hidden_entry_filtering() {
    tawc_integration::helpers::test_init();
    let _cleanup = Cleanup;

    let plant = format!(
        "mkdir -p \"$(dirname '{path}')\" && printf '%s\\n' \
         '[Desktop Entry]' 'Type=Application' 'Name=TAWC Hide Test' 'Exec=true' \
         > '{path}'",
        path = desktop_path()
    );
    assert_broker_ok(
        adb::rootfs_host_exec(&["/system/bin/sh", "-c", &plant]).expect("plant .desktop"),
        "plant .desktop",
    );
    // Clear any hide state a previously crashed run left behind.
    assert_broker_ok(
        adb::set_entry_hidden(ENTRY_ID, false).expect("set-entry-hidden"),
        "set-entry-hidden reset",
    );

    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, ENTRY_ID)
        .unwrap_or_else(|| panic!("planted entry missing from launcher-list: {list}"));
    assert!(
        obj.contains("\"hidden\":false"),
        "visible entry not flagged hidden:false: {obj}"
    );
    assert!(
        obj.contains(&format!("{ENTRY_ID}.desktop")),
        "entry object missing .desktop source path: {obj}"
    );

    // Hide: gone from the launcher-visible list...
    assert_broker_ok(
        adb::set_entry_hidden(ENTRY_ID, true).expect("set-entry-hidden"),
        "set-entry-hidden hide",
    );
    let list = adb::launcher_list(false).expect("launcher-list");
    assert!(
        entry_object(&list, ENTRY_ID).is_none(),
        "hidden entry still in default launcher-list: {list}"
    );
    // ...but present and flagged with the show-hidden toggle.
    let list = adb::launcher_list(true).expect("launcher-list showHidden");
    let obj = entry_object(&list, ENTRY_ID)
        .unwrap_or_else(|| panic!("hidden entry missing from showHidden list: {list}"));
    assert!(
        obj.contains("\"hidden\":true"),
        "hidden entry not flagged hidden:true: {obj}"
    );

    // Unhide: back in the default list.
    assert_broker_ok(
        adb::set_entry_hidden(ENTRY_ID, false).expect("set-entry-hidden"),
        "set-entry-hidden unhide",
    );
    let list = adb::launcher_list(false).expect("launcher-list");
    assert!(
        entry_object(&list, ENTRY_ID).is_some(),
        "unhidden entry missing from launcher-list: {list}"
    );
}

/// Unhides a built-in on drop (hide state is durable).
struct UnhideBuiltin(&'static str);

impl Drop for UnhideBuiltin {
    fn drop(&mut self) {
        let _ = adb::set_entry_hidden(self.0, false);
    }
}

/// The app's built-in entries (tawcroot install) are listed and flagged
/// `builtin`, and hide like any `.desktop` entry.
#[test]
fn test_builtin_entries_listed_and_hideable() {
    tawc_integration::helpers::test_init();
    let _cleanup = UnhideBuiltin("tawc:term");
    assert_broker_ok(
        adb::set_entry_hidden("tawc:term", false).expect("set-entry-hidden"),
        "set-entry-hidden reset",
    );
    let list = adb::launcher_list(false).expect("launcher-list");
    for id in ["tawc:term", "tawc:update", "tawc:add-entry"] {
        let obj = entry_object(&list, id).unwrap_or_else(|| panic!("{id} missing from launcher-list: {list}"));
        assert!(obj.contains("\"builtin\":true"), "{id} not flagged builtin: {obj}");
    }

    assert_broker_ok(
        adb::set_entry_hidden("tawc:term", true).expect("set-entry-hidden"),
        "set-entry-hidden hide",
    );
    let list = adb::launcher_list(false).expect("launcher-list");
    assert!(entry_object(&list, "tawc:term").is_none(), "hidden built-in still listed: {list}");
    let list = adb::launcher_list(true).expect("launcher-list showHidden");
    let obj = entry_object(&list, "tawc:term").unwrap_or_else(|| panic!("hidden built-in missing: {list}"));
    assert!(obj.contains("\"hidden\":true"), "built-in not flagged hidden: {obj}");
}

// ---- scan directories + precedence ------------------------------------

const USER_ID: &str = "tawc-scan-user";
const LOCAL_ID: &str = "tawc-scan-local";
const DUP_ID: &str = "tawc-scan-dup";

/// Remove every fixture `test_scan_dirs_precedence_and_terminal` plants,
/// on success and panic alike.
struct ScanCleanup;

impl Drop for ScanCleanup {
    fn drop(&mut self) {
        let r = rootfs();
        let rm = format!(
            "rm -f '{r}/root/.local/share/applications/{USER_ID}.desktop' \
                   '{r}/usr/local/share/applications/{LOCAL_ID}.desktop' \
                   '{r}/root/.local/share/applications/{DUP_ID}.desktop' \
                   '{r}/usr/share/applications/{DUP_ID}.desktop'"
        );
        let _ = adb::rootfs_host_exec(&["/system/bin/sh", "-c", &rm]);
    }
}

/// Plant a minimal `.desktop` at `<rootfs>/<subdir>/<id>.desktop`.
/// An empty `icon` omits the `Icon=` key.
fn plant_desktop(
    subdir: &str,
    id: &str,
    name: &str,
    exec: &str,
    terminal: bool,
    icon: &str,
) {
    let path = format!("{}/{subdir}/{id}.desktop", rootfs());
    let term = if terminal { " 'Terminal=true'" } else { "" };
    let icon = if icon.is_empty() {
        String::new()
    } else {
        format!(" 'Icon={icon}'")
    };
    let plant = format!(
        "mkdir -p \"$(dirname '{path}')\" && printf '%s\\n' \
         '[Desktop Entry]' 'Type=Application' 'Name={name}' 'Exec={exec}'{term}{icon} \
         > '{path}'"
    );
    assert_broker_ok(
        adb::rootfs_host_exec(&["/system/bin/sh", "-c", &plant]).expect("plant .desktop"),
        "plant .desktop",
    );
}

/// The scanner walks the user-writable dirs and gives them precedence:
/// entries in `/root/.local/share/applications` (the guest's XDG
/// per-user dir) and `/usr/local/share/applications` both appear,
/// `Terminal=true` is reported, and a duplicated id resolves to the
/// user copy — its name/exec shadow the `/usr/share` one
/// (notes/launcher.md "Scan directories").
#[test]
fn test_scan_dirs_precedence_and_terminal() {
    tawc_integration::helpers::test_init();
    let _cleanup = ScanCleanup;

    plant_desktop(
        "root/.local/share/applications",
        USER_ID,
        "TAWC Scan User",
        "tawc-scan-user-exec",
        true,
        "",
    );
    plant_desktop(
        "usr/local/share/applications",
        LOCAL_ID,
        "TAWC Scan Local",
        "tawc-scan-local-exec",
        false,
        "",
    );
    plant_desktop(
        "root/.local/share/applications",
        DUP_ID,
        "TAWC Scan Dup User",
        "tawc-scan-dup-user-exec",
        false,
        "",
    );
    plant_desktop(
        "usr/share/applications",
        DUP_ID,
        "TAWC Scan Dup Packaged",
        "tawc-scan-dup-packaged-exec",
        false,
        "",
    );

    let list = adb::launcher_list(false).expect("launcher-list");

    let user = entry_object(&list, USER_ID)
        .unwrap_or_else(|| panic!("per-user dir entry missing from launcher-list: {list}"));
    assert!(
        user.contains("\"terminal\":true"),
        "Terminal=true not reported: {user}"
    );
    // org.json escapes `/` as `\/` in launcher-list output.
    assert!(
        user.contains("\\/root\\/.local\\/share\\/applications\\/"),
        "per-user entry path not in the user dir: {user}"
    );

    assert!(
        entry_object(&list, LOCAL_ID).is_some(),
        "/usr/local entry missing from launcher-list: {list}"
    );

    let dup = entry_object(&list, DUP_ID)
        .unwrap_or_else(|| panic!("duplicated-id entry missing from launcher-list: {list}"));
    assert!(
        dup.contains("tawc-scan-dup-user-exec") && dup.contains("TAWC Scan Dup User"),
        "duplicated id did not resolve to the per-user copy: {dup}"
    );
    assert!(
        json_field(dup, "shadows").ends_with(&format!("/usr/share/applications/{DUP_ID}.desktop")),
        "per-user copy doesn't report the packaged file it shadows: {dup}"
    );
}

// ---- launcher store: overrides, shortcuts, migration ------------------

fn store_file() -> String {
    format!(
        "{}/distros/{}/launcher/entries.json",
        tawc_integration::app_data_dir(),
        tawc_integration::install_id()
    )
}

fn store_icons_dir() -> String {
    format!(
        "{}/distros/{}/launcher/icons",
        tawc_integration::app_data_dir(),
        tawc_integration::install_id()
    )
}

const OVERRIDE_ID: &str = "tawc-override-test";

struct OverrideCleanup;

impl Drop for OverrideCleanup {
    fn drop(&mut self) {
        for field in ["name", "exec", "graphics", "env", "icon"] {
            let _ = adb::set_entry_override(OVERRIDE_ID, field, None);
        }
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!("rm -f '{}/usr/share/applications/{OVERRIDE_ID}.desktop'", rootfs()),
        ]);
    }
}

/// Field overrides apply one by one over the scanned file: the
/// overridden name and graphics win, the scanned value is reported as
/// `packaged`, and a package upgrade (new Exec) still reaches the fields
/// that aren't overridden. Clearing a field brings the scanned value back.
/// No `.desktop` file is written.
#[test]
fn test_field_overrides_follow_package_upgrades() {
    tawc_integration::helpers::test_init();
    let _cleanup = OverrideCleanup;

    plant_desktop("usr/share/applications", OVERRIDE_ID, "TAWC Packaged", "tawc-v1", false, "");
    for (field, value) in [("name", "TAWC Mine"), ("graphics", "cpu"), ("env", r#"{"TAWC_A":"1"}"#)] {
        assert_broker_ok(
            adb::set_entry_override(OVERRIDE_ID, field, Some(value)).expect("set-entry-override"),
            "set-entry-override",
        );
    }
    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, OVERRIDE_ID).unwrap_or_else(|| panic!("entry missing: {list}"));
    assert_eq!(json_field(obj, "name"), "TAWC Mine", "name override: {obj}");
    assert_eq!(json_field(obj, "graphics"), "cpu", "graphics override: {obj}");
    assert_eq!(json_field(obj, "source"), "desktop", "{obj}");
    assert_eq!(json_field(obj, "exec"), "tawc-v1", "{obj}");
    let packaged = raw_field(obj, "packaged").unwrap_or_default();
    assert_eq!(json_field(packaged, "name"), "TAWC Packaged", "packaged name: {obj}");
    let overridden = raw_field(obj, "overridden").unwrap_or_default();
    for f in ["\"name\"", "\"graphics\"", "\"env\""] {
        assert!(overridden.contains(f), "{f} not in overridden: {obj}");
    }
    assert!(raw_field(obj, "env").unwrap_or_default().contains("\"TAWC_A\":\"1\""), "env: {obj}");
    assert_eq!(
        host_sh(&format!("ls '{}/root/.local/share/applications/{OVERRIDE_ID}.desktop' 2>/dev/null", rootfs())),
        "",
        "an override wrote a .desktop file"
    );

    // "Upgrade" the package: the override survives, Exec follows.
    plant_desktop("usr/share/applications", OVERRIDE_ID, "TAWC Packaged 2", "tawc-v2", false, "");
    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, OVERRIDE_ID).expect("entry");
    assert_eq!(json_field(obj, "name"), "TAWC Mine", "override lost on upgrade: {obj}");
    assert_eq!(json_field(obj, "exec"), "tawc-v2", "upgrade didn't reach Exec: {obj}");
    assert_eq!(
        json_field(raw_field(obj, "packaged").unwrap_or_default(), "name"),
        "TAWC Packaged 2",
        "{obj}"
    );

    assert_broker_ok(
        adb::set_entry_override(OVERRIDE_ID, "name", None).expect("set-entry-override"),
        "clear name",
    );
    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, OVERRIDE_ID).expect("entry");
    assert_eq!(json_field(obj, "name"), "TAWC Packaged 2", "cleared name not back: {obj}");
    assert_eq!(json_field(obj, "graphics"), "cpu", "other override lost: {obj}");
}

const SHORTCUT_DUP: &str = "tawc-shortcut-dup";
const RESERVED_FILE: &str = "tawc:reserved";

struct ShortcutCleanup(Vec<String>);

impl Drop for ShortcutCleanup {
    fn drop(&mut self) {
        for id in &self.0 {
            let _ = adb::delete_shortcut(id);
        }
        let _ = adb::delete_shortcut(SHORTCUT_DUP);
        let r = rootfs();
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -f '{r}/usr/share/applications/{SHORTCUT_DUP}.desktop' \
                       '{r}/usr/share/applications/{RESERVED_FILE}.desktop' \
                       '{icons}/tawc-test-icon.png'",
                icons = store_icons_dir()
            ),
        ]);
    }
}

/// Shortcuts: listed with `source: shortcut` under a `tawc:app:` id,
/// resolve a store `iconFile` (and refuse one that climbs out of
/// `icons/`), win over a scanned entry with the same id, and go away on
/// delete. A scanned `.desktop` id with the reserved prefix is dropped.
#[test]
fn test_shortcuts() {
    tawc_integration::helpers::test_init();
    let mut cleanup = ShortcutCleanup(Vec::new());

    host_sh_ok(&format!("mkdir -p '{}'", store_icons_dir()), "mkdir icons");
    plant_png(&format!("{}/tawc-test-icon.png", store_icons_dir()));
    let id = adb::put_shortcut(
        None,
        r#"{"name":"TAWC Shortcut","exec":"tawc-shortcut-exec","terminal":true,"iconFile":"tawc-test-icon.png"}"#,
    )
    .expect("put-shortcut");
    cleanup.0.push(id.clone());
    assert!(id.starts_with("tawc:app:"), "new shortcut id: {id}");

    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, &id).unwrap_or_else(|| panic!("shortcut missing: {list}"));
    assert_eq!(json_field(obj, "source"), "shortcut", "{obj}");
    assert_eq!(json_field(obj, "exec"), "tawc-shortcut-exec", "{obj}");
    assert_eq!(raw_field(obj, "terminal"), Some("true"), "{obj}");
    assert_eq!(
        json_field(obj, "iconPath"),
        format!("{}/tawc-test-icon.png", store_icons_dir()),
        "store icon not resolved: {obj}"
    );

    let escape = adb::put_shortcut(
        None,
        r#"{"name":"TAWC Escape","exec":"x","iconFile":"../entries.json","icon":"/../../../system/etc/hosts"}"#,
    )
    .expect("put-shortcut");
    cleanup.0.push(escape.clone());
    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, &escape).expect("escape shortcut");
    assert_eq!(json_field(obj, "iconPath"), "", "iconFile escaped icons/: {obj}");

    // A shortcut replaces a scanned entry with the same id.
    plant_desktop("usr/share/applications", SHORTCUT_DUP, "TAWC Scanned", "scanned", false, "");
    adb::put_shortcut(Some(SHORTCUT_DUP), r#"{"name":"TAWC Shortcut Dup","exec":"mine"}"#).expect("put-shortcut");
    let list = adb::launcher_list(false).expect("launcher-list");
    assert_eq!(list.matches(&format!("\"id\":\"{SHORTCUT_DUP}\"")).count(), 1, "{list}");
    let obj = entry_object(&list, SHORTCUT_DUP).expect("dup");
    assert_eq!(json_field(obj, "name"), "TAWC Shortcut Dup", "shortcut didn't win: {obj}");

    // Reserved prefix on a scanned file: dropped.
    plant_desktop("usr/share/applications", RESERVED_FILE, "TAWC Reserved", "x", false, "");
    let list = adb::launcher_list(false).expect("launcher-list");
    assert!(entry_object(&list, RESERVED_FILE).is_none(), "reserved scanned id listed: {list}");

    // Delete: gone, and its icon file pruned.
    assert_broker_ok(adb::delete_shortcut(&id).expect("delete-shortcut"), "delete-shortcut");
    let list = adb::launcher_list(false).expect("launcher-list");
    assert!(entry_object(&list, &id).is_none(), "deleted shortcut listed: {list}");
    assert_eq!(
        host_sh(&format!("test -e '{}/tawc-test-icon.png' && echo present || echo gone", store_icons_dir())),
        "gone",
        "unreferenced icon not pruned"
    );
}

const MIG_PERSONAL: &str = "tawc-mig-personal";
const MIG_HAND: &str = "tawc-mig-hand";
const MIG_ICON: &str = "tawc-mig-icon";

fn user_apps_dir() -> String {
    format!("{}/root/.local/share/applications", rootfs())
}

struct MigrationCleanup;

impl Drop for MigrationCleanup {
    fn drop(&mut self) {
        let _ = adb::delete_shortcut(MIG_PERSONAL);
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -f '{apps}/{MIG_PERSONAL}.desktop' '{apps}/{MIG_HAND}.desktop' \
                       '{user}/hicolor/256x256/apps/{MIG_ICON}.png'",
                apps = user_apps_dir(),
                user = user_icons_dir(),
            ),
        ]);
    }
}

/// Migration of the old editor's `.desktop` files: a v3-shaped personal
/// entry becomes a shortcut under its old id (so a pin keeps resolving),
/// its `env` prefix and `tawc-*` icon move into the store, and its file
/// is deleted; a hand-made file with extra keys is left alone.
#[test]
fn test_migration_converts_editor_files_only() {
    tawc_integration::helpers::test_init();
    let _cleanup = MigrationCleanup;

    // Make sure the store exists, then seed v3 state and re-arm migration.
    adb::launcher_list(false).expect("launcher-list");
    plant_png(&format!("{}/hicolor/256x256/apps/{MIG_ICON}.png", user_icons_dir()));
    host_sh_ok(
        &format!(
            "printf '%s\\n' '[Desktop Entry]' 'Type=Application' 'Name=TAWC Mig' \
             'Exec=env TAWC_M=\"a b\" tawc-mig-exec' 'Icon={MIG_ICON}' 'Terminal=true' \
             > '{apps}/{MIG_PERSONAL}.desktop' && \
             printf '%s\\n' '[Desktop Entry]' 'Type=Application' 'Name=TAWC Hand' 'Exec=tawc-hand' \
             'Categories=Utility;' > '{apps}/{MIG_HAND}.desktop' && \
             sed -i 's/\"migrated\": 1/\"migrated\": 0/' '{store}'",
            apps = user_apps_dir(),
            store = store_file(),
        ),
        "seed v3 state",
    );

    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, MIG_PERSONAL).unwrap_or_else(|| panic!("personal entry missing: {list}"));
    assert_eq!(json_field(obj, "source"), "shortcut", "not converted: {obj}");
    assert_eq!(json_field(obj, "exec"), "tawc-mig-exec", "{obj}");
    assert!(raw_field(obj, "env").unwrap_or_default().contains("\"TAWC_M\":\"a b\""), "env: {obj}");
    assert_eq!(json_field(obj, "iconFile"), format!("{MIG_ICON}.png"), "icon not moved: {obj}");
    assert!(json_field(obj, "iconPath").starts_with(&store_icons_dir()), "{obj}");
    assert_eq!(
        host_sh(&format!("test -e '{}/{MIG_PERSONAL}.desktop' && echo present || echo gone", user_apps_dir())),
        "gone",
        "converted file not deleted"
    );

    let hand = entry_object(&list, MIG_HAND).unwrap_or_else(|| panic!("hand-made entry missing: {list}"));
    assert_eq!(json_field(hand, "source"), "desktop", "hand-made file converted: {hand}");
    assert!(host_sh(&format!("cat '{}'", store_file())).contains("\"migrated\": 1"));
}

// ---- icon resolution --------------------------------------------------

/// Cache dir `launcher.rs` rasterizes SVG icons into — a sibling of the
/// rootfs, so it belongs to the install and goes away with it.
fn icon_cache_dir() -> String {
    format!(
        "{}/distros/{}/icon-cache",
        tawc_integration::app_data_dir(),
        tawc_integration::install_id()
    )
}

fn icons_dir() -> String {
    format!("{}/usr/share/icons", rootfs())
}

/// Run a host-side shell command as the app uid and return its stdout,
/// trimmed. Panics if the broker call fails; a non-zero command exit is
/// the caller's business (missing files are a normal answer here).
fn host_sh(cmd: &str) -> String {
    let out = adb::rootfs_host_exec(&["/system/bin/sh", "-c", cmd]).expect("host sh");
    String::from_utf8_lossy(&out.stdout).trim().to_string()
}

/// Same, but the command must succeed.
fn host_sh_ok(cmd: &str, what: &str) {
    assert_broker_ok(
        adb::rootfs_host_exec(&["/system/bin/sh", "-c", cmd]).expect(what),
        what,
    );
}

/// Value of a top-level string field in a `launcher-list` object, with
/// org.json's `\/` and `\"` escaping undone. Empty when absent.
fn json_field(obj: &str, key: &str) -> String {
    match raw_field(obj, key) {
        Some(v) if v.starts_with('"') => v[1..v.len() - 1].replace("\\/", "/").replace("\\\"", "\""),
        _ => String::new(),
    }
}

/// `iconPath` of the entry with this id. Panics if the entry is absent —
/// every icon test plants its entry first.
fn icon_path_of(id: &str) -> String {
    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, id)
        .unwrap_or_else(|| panic!("entry {id} missing from launcher-list: {list}"));
    json_field(obj, "iconPath")
}

/// Write an SVG of `size`² into the rootfs at `path`.
fn plant_svg(path: &str, size: u32) {
    let svg = format!(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{size}\" height=\"{size}\">\
         <rect width=\"{size}\" height=\"{size}\" fill=\"#ff0000\"/></svg>"
    );
    host_sh_ok(
        &format!("mkdir -p \"$(dirname '{path}')\" && printf '%s' '{svg}' > '{path}'"),
        "plant svg",
    );
}

/// A 4×4 red PNG, so a PNG-vs-SVG preference test doesn't need a
/// renderer on the host side.
const FIXTURE_PNG_B64: &str = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAEklEQVR4nGP4z8DwHxk\
                               zkC4AADxAH+HggXe0AAAAAElFTkSuQmCC";

fn plant_png(path: &str) {
    host_sh_ok(
        &format!(
            "mkdir -p \"$(dirname '{path}')\" && printf '%s' '{FIXTURE_PNG_B64}' \
             | base64 -d > '{path}'"
        ),
        "plant png",
    );
}

const SVG_ICON_ID: &str = "tawc-icon-svg";
const SVG_ICON_NAME: &str = "tawc-icon-svg-name";

struct SvgIconCleanup;

impl Drop for SvgIconCleanup {
    fn drop(&mut self) {
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -f '{}/usr/share/applications/{SVG_ICON_ID}.desktop' \
                       '{}/hicolor/scalable/apps/{SVG_ICON_NAME}.svg'",
                rootfs(),
                icons_dir()
            ),
        ]);
    }
}

/// An SVG-only icon resolves to a PNG in the per-install cache, the
/// cache is a hit on the next scan, and rewriting the source rotates the
/// cached file and prunes the stale one.
#[test]
fn test_svg_icon_rasterized_and_cached() {
    tawc_integration::helpers::test_init();
    let _cleanup = SvgIconCleanup;

    let svg = format!("{}/hicolor/scalable/apps/{SVG_ICON_NAME}.svg", icons_dir());
    plant_svg(&svg, 32);
    plant_desktop(
        "usr/share/applications",
        SVG_ICON_ID,
        "TAWC Icon SVG",
        "tawc-icon-svg-exec",
        false,
        SVG_ICON_NAME,
    );

    let path = icon_path_of(SVG_ICON_ID);
    assert!(
        path.ends_with(".png") && path.starts_with(&icon_cache_dir()),
        "SVG icon did not resolve into the cache as a PNG: {path:?}"
    );
    // Decodable by BitmapFactory means: actually a PNG.
    let magic = host_sh(&format!("od -An -tx1 -N4 '{path}'"));
    assert_eq!(
        magic.split_whitespace().collect::<Vec<_>>(),
        vec!["89", "50", "4e", "47"],
        "cached icon is not a PNG: {path}"
    );

    // Second scan: same key, and the file was not rewritten.
    let mtime = host_sh(&format!("stat -c %Y '{path}'"));
    let again = icon_path_of(SVG_ICON_ID);
    assert_eq!(again, path, "cache key changed with no source change");
    assert_eq!(
        host_sh(&format!("stat -c %Y '{path}'")),
        mtime,
        "cached icon re-rendered on a cache hit: {path}"
    );

    // Source rewritten: new cache entry, old one pruned.
    plant_svg(&svg, 48);
    let rotated = icon_path_of(SVG_ICON_ID);
    assert_ne!(rotated, path, "cache key survived a source rewrite");
    assert_eq!(
        host_sh(&format!("test -e '{path}' && echo present || echo gone")),
        "gone",
        "stale cache entry not pruned: {path}"
    );
}

const BAD_ICON_ID: &str = "tawc-icon-bad";
const BAD_ICON_NAME: &str = "tawc-icon-bad-name";

struct BadIconCleanup;

impl Drop for BadIconCleanup {
    fn drop(&mut self) {
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -f '{}/usr/share/applications/{BAD_ICON_ID}.desktop' \
                       '{}/hicolor/scalable/apps/{BAD_ICON_NAME}.svg'",
                rootfs(),
                icons_dir()
            ),
        ]);
    }
}

/// A malformed SVG is not fatal: the entry keeps its place in the list
/// with an empty `iconPath` (Kotlin draws the fallback glyph), and a
/// `.fail` marker keeps the next scan from re-parsing it.
#[test]
fn test_malformed_svg_icon_is_not_fatal() {
    tawc_integration::helpers::test_init();
    let _cleanup = BadIconCleanup;

    let svg = format!("{}/hicolor/scalable/apps/{BAD_ICON_NAME}.svg", icons_dir());
    host_sh_ok(
        &format!(
            "mkdir -p \"$(dirname '{svg}')\" && printf '%s' '<svg not xml at all' > '{svg}'"
        ),
        "plant malformed svg",
    );
    plant_desktop(
        "usr/share/applications",
        BAD_ICON_ID,
        "TAWC Icon Bad",
        "tawc-icon-bad-exec",
        false,
        BAD_ICON_NAME,
    );

    let list = adb::launcher_list(false).expect("launcher-list");
    let obj = entry_object(&list, BAD_ICON_ID)
        .unwrap_or_else(|| panic!("entry with a bad icon dropped from the list: {list}"));
    assert_eq!(
        json_field(obj, "iconPath"),
        "",
        "malformed SVG resolved to a path: {obj}"
    );
    // The rest of the scan is unaffected.
    assert!(
        list.matches("\"id\":").count() > 1,
        "scan returned only the bad entry: {list}"
    );
    assert_ne!(
        host_sh(&format!("ls '{}' | grep -c '\\.fail$'", icon_cache_dir())),
        "0",
        "no .fail marker left for the malformed SVG"
    );
}

const THEME_ICON_ID: &str = "tawc-icon-theme";
const INHERIT_ICON_NAME: &str = "tawc-icon-inherit-name";
const BREEZE_ICON_ID: &str = "tawc-icon-breeze";
const BREEZE_ICON_NAME: &str = "tawc-icon-breeze-name";
const SYMBOLIC_ICON_ID: &str = "tawc-icon-symbolic";
const SYMBOLIC_ICON_NAME: &str = "tawc-icon-symbolic-name";
const PARENT_THEME: &str = "tawc-test-parent";

/// Seed theme to plant the fixture under. Must not already exist —
/// overwriting a distro's real theme is not something a test gets to do.
fn free_seed_theme() -> Option<&'static str> {
    ["Papirus", "breeze"].into_iter().find(|theme| {
        host_sh(&format!(
            "test -d '{}/{theme}' && echo present || echo free",
            icons_dir()
        )) == "free"
    })
}

struct ThemeCleanup(Option<&'static str>);

impl Drop for ThemeCleanup {
    fn drop(&mut self) {
        let seed = self.0.unwrap_or("");
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -rf '{icons}/{PARENT_THEME}' {seed_dir}; \
                 rm -f '{root}/usr/share/applications/{THEME_ICON_ID}.desktop' \
                       '{root}/usr/share/applications/{BREEZE_ICON_ID}.desktop' \
                       '{root}/usr/share/applications/{SYMBOLIC_ICON_ID}.desktop'",
                icons = icons_dir(),
                root = rootfs(),
                seed_dir = if seed.is_empty() {
                    String::new()
                } else {
                    format!("'{}/{seed}'", icons_dir())
                },
            ),
        ]);
    }
}

/// The theme walk follows `index.theme` `Inherits=` chains, handles the
/// `<context>/<size>` layout breeze uses (not just `<size>/<context>`),
/// and falls back to a tiled symbolic glyph when a name has nothing
/// else.
#[test]
fn test_theme_inherits_and_context_size_layout() {
    tawc_integration::helpers::test_init();

    let Some(seed) = free_seed_theme() else {
        eprintln!("skipping: every seed theme is already installed in this rootfs");
        return;
    };
    let _cleanup = ThemeCleanup(Some(seed));

    // The seed theme has no icons of its own but inherits the parent...
    host_sh_ok(
        &format!(
            "mkdir -p '{icons}/{seed}' && printf '%s\\n' '[Icon Theme]' \
             'Name={seed}' 'Inherits={PARENT_THEME},hicolor' \
             > '{icons}/{seed}/index.theme'",
            icons = icons_dir()
        ),
        "plant seed theme",
    );
    plant_png(&format!(
        "{}/{PARENT_THEME}/48x48/apps/{INHERIT_ICON_NAME}.png",
        icons_dir()
    ));
    plant_desktop(
        "usr/share/applications",
        THEME_ICON_ID,
        "TAWC Icon Inherited",
        "tawc-icon-theme-exec",
        false,
        INHERIT_ICON_NAME,
    );

    // ...and the seed itself carries a breeze-layout icon.
    plant_png(&format!(
        "{}/{seed}/apps/48/{BREEZE_ICON_NAME}.png",
        icons_dir()
    ));
    plant_desktop(
        "usr/share/applications",
        BREEZE_ICON_ID,
        "TAWC Icon Breeze Layout",
        "tawc-icon-breeze-exec",
        false,
        BREEZE_ICON_NAME,
    );

    // ...and a name that exists only as a symbolic glyph.
    plant_svg(
        &format!(
            "{}/{seed}/symbolic/apps/{SYMBOLIC_ICON_NAME}-symbolic.svg",
            icons_dir()
        ),
        16,
    );
    plant_desktop(
        "usr/share/applications",
        SYMBOLIC_ICON_ID,
        "TAWC Icon Symbolic",
        "tawc-icon-symbolic-exec",
        false,
        SYMBOLIC_ICON_NAME,
    );

    let inherited = icon_path_of(THEME_ICON_ID);
    assert!(
        inherited.ends_with(&format!("{PARENT_THEME}/48x48/apps/{INHERIT_ICON_NAME}.png")),
        "Inherits= chain not followed: {inherited:?}"
    );
    let breeze = icon_path_of(BREEZE_ICON_ID);
    assert!(
        breeze.ends_with(&format!("{seed}/apps/48/{BREEZE_ICON_NAME}.png")),
        "<context>/<size> layout not searched: {breeze:?}"
    );
    let symbolic = icon_path_of(SYMBOLIC_ICON_ID);
    assert!(
        symbolic.starts_with(&icon_cache_dir()) && symbolic.ends_with(".png"),
        "symbolic-only icon did not resolve to a tiled rendering: {symbolic:?}"
    );
}

const ESCAPE_ICON_ID: &str = "tawc-icon-escape";

struct EscapeCleanup;

impl Drop for EscapeCleanup {
    fn drop(&mut self) {
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -f '{}/usr/share/applications/{ESCAPE_ICON_ID}.desktop'",
                rootfs()
            ),
        ]);
    }
}

/// `Icon=` is guest-controlled and the resolver now *parses* what it
/// finds, so an absolute path that climbs out of the rootfs resolves to
/// nothing rather than to a host file.
#[test]
fn test_absolute_icon_path_cannot_escape_rootfs() {
    tawc_integration::helpers::test_init();
    let _cleanup = EscapeCleanup;

    plant_desktop(
        "usr/share/applications",
        ESCAPE_ICON_ID,
        "TAWC Icon Escape",
        "tawc-icon-escape-exec",
        false,
        "/../../../../system/etc/escape.png",
    );
    assert_eq!(
        icon_path_of(ESCAPE_ICON_ID),
        "",
        "absolute Icon= escaped the rootfs"
    );
}

// ---- icon picker: listing + one-value resolve -------------------------

const PICK_USER: &str = "tawc-pick-user";
const PICK_512: &str = "tawc-pick-512";
const PICK_PLACE: &str = "tawc-pick-place";
const PICK_CTX: &str = "tawc-pick-ctx";
const PICK_TWO: &str = "tawc-pick-two";

fn user_icons_dir() -> String {
    format!("{}/root/.local/share/icons", rootfs())
}

struct PickCleanup(Option<&'static str>);

impl Drop for PickCleanup {
    fn drop(&mut self) {
        let user = user_icons_dir();
        let seed_dir = match self.0 {
            Some(seed) => format!("'{}/{seed}'", icons_dir()),
            None => String::new(),
        };
        // Our files only; the user base may hold real imports, so its
        // dirs are removed only if this left them empty.
        let _ = adb::rootfs_host_exec(&[
            "/system/bin/sh",
            "-c",
            &format!(
                "rm -rf {seed_dir}; \
                 rm -f '{user}/hicolor/48x48/apps/{PICK_USER}.png' \
                       '{sys}/hicolor/48x48/apps/{PICK_USER}.png' \
                       '{sys}/hicolor/512x512/apps/{PICK_512}.png' \
                       '{sys}/hicolor/48x48/places/{PICK_PLACE}.png' \
                       '{sys}/hicolor/48x48/apps/{PICK_CTX}.png' \
                       '{sys}/hicolor/256x256/mimetypes/{PICK_CTX}.png' \
                       '{sys}/hicolor/48x48/apps/{PICK_TWO}.png'; \
                 rmdir '{user}/hicolor/48x48/apps' '{user}/hicolor/48x48' \
                       '{user}/hicolor' '{user}' 2>/dev/null; true",
                sys = icons_dir(),
            ),
        ]);
    }
}

/// Every `{"name":…}` object for `name` in a `launcher-icons` list.
fn icon_entries<'a>(list: &'a str, name: &str) -> Vec<&'a str> {
    let needle = format!("\"name\":\"{name}\"");
    list.match_indices(&needle)
        .map(|(i, _)| {
            let start = list[..i].rfind('{').expect("object start");
            let end = i + list[i..].find('}').expect("object end");
            &list[start..=end]
        })
        .collect()
}

fn resolve(value: &str) -> String {
    adb::launcher_resolve_icon(value).expect("launcher-resolve-icon")
}

/// The resolver changes behind the editor's icon picker: the user icon
/// base is searched and wins, 512-only and non-app-context icons
/// resolve, an app icon beats a same-named generic one, a name in two
/// themes is listed once, and absolute paths still work.
#[test]
fn test_icon_picker_resolver() {
    tawc_integration::helpers::test_init();
    let seed = free_seed_theme();
    let _cleanup = PickCleanup(seed);

    plant_png(&format!("{}/hicolor/48x48/apps/{PICK_USER}.png", user_icons_dir()));
    plant_png(&format!("{}/hicolor/48x48/apps/{PICK_USER}.png", icons_dir()));
    plant_png(&format!("{}/hicolor/512x512/apps/{PICK_512}.png", icons_dir()));
    plant_png(&format!("{}/hicolor/48x48/places/{PICK_PLACE}.png", icons_dir()));
    plant_png(&format!("{}/hicolor/48x48/apps/{PICK_CTX}.png", icons_dir()));
    plant_png(&format!("{}/hicolor/256x256/mimetypes/{PICK_CTX}.png", icons_dir()));
    plant_png(&format!("{}/hicolor/48x48/apps/{PICK_TWO}.png", icons_dir()));
    if let Some(seed) = seed {
        plant_png(&format!("{}/{seed}/48x48/apps/{PICK_TWO}.png", icons_dir()));
    }

    let user = resolve(PICK_USER);
    assert!(
        user.ends_with(&format!("root/.local/share/icons/hicolor/48x48/apps/{PICK_USER}.png")),
        "user icon base not searched first: {user:?}"
    );
    let big = resolve(PICK_512);
    assert!(big.ends_with(&format!("512x512/apps/{PICK_512}.png")), "512-only icon: {big:?}");
    let place = resolve(PICK_PLACE);
    assert!(place.ends_with(&format!("places/{PICK_PLACE}.png")), "places icon: {place:?}");
    let ctx = resolve(PICK_CTX);
    assert!(
        ctx.ends_with(&format!("48x48/apps/{PICK_CTX}.png")),
        "generic context beat apps: {ctx:?}"
    );
    if let Some(seed) = seed {
        let two = resolve(PICK_TWO);
        assert!(
            two.ends_with(&format!("{seed}/48x48/apps/{PICK_TWO}.png")),
            "seed theme did not beat hicolor: {two:?}"
        );
    }
    let abs = resolve(&format!("/usr/share/icons/hicolor/512x512/apps/{PICK_512}.png"));
    assert!(abs.ends_with(&format!("512x512/apps/{PICK_512}.png")), "absolute path: {abs:?}");

    let list = adb::launcher_icons().expect("launcher-icons");
    for name in [PICK_USER, PICK_512, PICK_PLACE, PICK_CTX, PICK_TWO] {
        let entries = icon_entries(&list, name);
        assert_eq!(entries.len(), 1, "{name} not listed exactly once: {entries:?}");
    }
    assert!(
        icon_entries(&list, PICK_USER)[0].contains("\"user\":true"),
        "user-base icon not flagged"
    );
    assert!(
        icon_entries(&list, PICK_PLACE)[0].contains("\"user\":false"),
        "system icon flagged as user"
    );
}

/// Everything the picker lists resolves to a PNG through the same
/// resolver, sampled across the install's real icon set.
#[test]
fn test_listed_icons_resolve() {
    tawc_integration::helpers::test_init();
    let list = adb::launcher_icons().expect("launcher-icons");
    let names: Vec<&str> = list
        .match_indices("\"name\":\"")
        .map(|(i, m)| {
            let start = i + m.len();
            &list[start..start + list[start..].find('"').expect("unterminated name")]
        })
        .collect();
    assert!(!names.is_empty(), "no icons listed: {list}");
    const SAMPLES: usize = 40;
    let step = (names.len() / SAMPLES).max(1);
    for name in names.iter().step_by(step) {
        let path = resolve(name);
        assert!(path.ends_with(".png"), "listed icon {name:?} did not resolve: {path:?}");
    }
}
