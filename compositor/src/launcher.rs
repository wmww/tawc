//! Desktop-entry scanner for the in-app launcher.
//!
//! Walks the standard XDG `applications/` directories *inside* a given
//! rootfs (e.g. `<appData>/distros/<id>/rootfs/usr/share/applications`)
//! and returns parsed `.desktop` entries. The list is consumed by Kotlin
//! via JNI in `lib.rs`; entries are JSON-encoded so the Java boundary
//! stays trivially small.
//!
//! Filtering matches what a normal Linux app menu shows: `Type=Application`,
//! not `NoDisplay`, not `Hidden`, has an `Exec`. The `OnlyShowIn` /
//! `NotShowIn` machinery is intentionally ignored — a desktop session inside
//! the chroot has no canonical name (we're "TAWC", not "GNOME"), and almost
//! every entry that uses these keys still works fine.
//!
//! Icon resolution: we map `Icon=` to an absolute on-device PNG path
//! inside the rootfs, walking the installed icon themes (including
//! their `index.theme` `Inherits=` chains) roughly the way the fdo Icon
//! Theme Spec says to. Only PNG comes back — Android's `BitmapFactory`
//! can't decode SVG/XPM natively, so any other path would just produce
//! a broken row — and SVG sources are rasterized into a per-install PNG
//! cache first (see [crate::icon_cache]). Resolution is lazy:
//! `scan_entries` keeps the raw `Icon=` value, and only the caller that
//! needs a path pays for the walk. See `notes/launcher.md`.

use std::collections::{HashMap, HashSet, VecDeque};
use std::io::{BufRead, BufReader};
use std::path::{Component, Path, PathBuf};

use freedesktop_desktop_entry::{DesktopEntry, Iter};
use log::warn;
use serde_json::json;

use crate::icon_cache::IconCache;

/// Directories under a rootfs that may contain `.desktop` files. Mirrors
/// `$XDG_DATA_HOME` + `$XDG_DATA_DIRS` for a standard glibc install
/// (the guest runs as fake root, so `$HOME` is `/root`) plus flatpak's
/// exports. Order is de-dup priority: earlier dirs win for a duplicated
/// id, so a user's `.desktop` in `/root/.local` shadows the packaged
/// copy — required for "hide the packaged entry behind my edited copy".
const APPS_SUBDIRS: &[&str] = &[
    "root/.local/share/applications",
    "usr/local/share/applications",
    "usr/share/applications",
    "var/lib/flatpak/exports/share/applications",
    "var/lib/snapd/desktop/applications",
];

/// Themes seeded into the search under `<rootfs>/usr/share/icons`,
/// before `Inherits=` expansion. `default` first: distros symlink or
/// copy it to the session's chosen theme, so it should win where it
/// exists. The middle three are the common shipped themes — a themed
/// icon usually looks nicer than hicolor's generic one. `hicolor` is
/// the spec's universal fallback and is forced *last* in the resolved
/// order (see [resolve_theme_order]) no matter where it appears here,
/// so an inherited parent theme still gets a look in first.
const ICON_THEMES: &[&str] = &["default", "Adwaita", "Papirus", "breeze", "hicolor"];

/// Icon base dirs under a rootfs, in lookup order: `$XDG_DATA_HOME`
/// (`/root`, see [APPS_SUBDIRS]) first, as the icon theme spec
/// requires, then `$XDG_DATA_DIRS`, then flatpak's exports, whose apps
/// [APPS_SUBDIRS] already lists. A theme may span several bases.
const ICON_BASES: &[&str] = &[
    "root/.local/share/icons",
    "usr/local/share/icons",
    "usr/share/icons",
    "var/lib/flatpak/exports/share/icons",
];

/// Index of the user base in [ICON_BASES]; [list_icons_json] flags
/// names found there.
const USER_ICON_BASE: usize = 0;

/// Preferred icon-theme contexts, in order. Applications ship into
/// `apps`; the generic names apps reference instead of their own icon
/// (`utilities-terminal`, `system-file-manager`, …) live in `legacy` or
/// `categories` in several themes. Every other context the theme has
/// (`places`, `mimetypes`, …) is searched after these, so an app's own
/// icon beats a same-named generic one.
const ICON_CONTEXTS: &[&str] = &["apps", "legacy", "categories"];

/// Size directories to try, in preference order, as they appear in a
/// theme (`128x128` and the bare `128` breeze uses are both tried).
/// Mid-size first because list rows render at ~56 dp (~168 px on a 3×
/// density phone) — a 128 PNG scales cleanly to that without burning
/// the memory of a 256. `scalable` sits between the big and the small
/// sizes: a vector icon beats a 16 px PNG blown up to a launcher row.
/// Any other size dir a theme has (`512x512`, `36`, `48x48@2`, …) is
/// tried after these, largest first.
const ICON_SIZES: &[&str] = &[
    "128", "96", "256", "64", "48", "scalable", "32", "24", "22", "16",
];

/// Image extensions recognised in `Icon=foo.png` style entries. Used
/// only to strip a known extension before doing the theme search; the
/// resolver itself only ever returns `.png` paths.
const KNOWN_ICON_EXTS: &[&str] = &["png", "svg", "xpm", "jpg", "jpeg"];

/// File extensions accepted inside a theme directory, in preference
/// order. PNG first so a theme that ships both costs no rendering;
/// SVG/SVGZ go through [icon_cache]. XPM is deliberately absent —
/// nothing we can decode, and only a couple of `NoDisplay` python
/// entries still use it.
const ICON_FILE_EXTS: &[&str] = &["png", "svg", "svgz"];

/// One launchable application, ready to ship to Kotlin.
pub struct Entry {
    /// Filename minus `.desktop` — the stable id Kotlin hide-state and
    /// shortcut pins reference; `resolve_metadata_for_app_id` matches
    /// window app_ids against it.
    pub id: String,
    pub name: String,
    pub comment: String,
    /// Raw `Exec=` line. Field codes (`%f`, `%u`, …) stripped because we
    /// don't pass URIs at launch time; everything else (quoting, env
    /// vars) is left for `bash -lc` to handle.
    pub exec: String,
    pub terminal: bool,
    /// Raw `Icon=` value, unresolved. Resolution is deliberately lazy
    /// ([IconResolver::resolve]): it stats its way through the icon
    /// themes and, for SVG sources, rasterizes — so doing it for every
    /// entry of a scan whose caller wants one of them is pure waste.
    pub icon: String,
    /// Absolute host path of the parsed `.desktop` file. Lets Kotlin
    /// decide whether an entry is user-editable (managed dir) without
    /// re-deriving the scan layout.
    pub path: String,
    /// Path of the lower-priority copy of this id that the de-dup
    /// dropped (e.g. the packaged file a managed-dir override hides),
    /// or empty. The editor's Reset-vs-Delete choice keys on it.
    pub shadows: String,
}

/// Scan [rootfs] for `.desktop` apps. Returns entries sorted by name
/// (case-insensitive), de-duplicated by id — the per-user dir wins over
/// `/usr/share` if both ship the same id, matching desktop-environment
/// convention. `launchable_only` additionally drops `NoDisplay` entries
/// (the launcher list); metadata resolution keeps them — a *running*
/// NoDisplay app still needs its icon/title resolved.
fn scan_entries(rootfs: &Path, launchable_only: bool) -> Vec<Entry> {
    let dirs: Vec<PathBuf> = APPS_SUBDIRS
        .iter()
        .map(|sub| rootfs.join(sub))
        .filter(|p| p.exists())
        .collect();

    let mut entries: Vec<Entry> = Vec::new();
    let locales = current_locales();
    for path in Iter::new(dirs.into_iter()) {
        let de = match DesktopEntry::from_path(path, Some(&locales)) {
            Ok(de) => de,
            Err(_) => continue,
        };
        if !is_relevant(&de, launchable_only) {
            continue;
        }
        let exec = match de.exec() {
            Some(e) if !e.trim().is_empty() => strip_field_codes(e),
            _ => continue,
        };
        let name = de
            .name(&locales)
            .map(|s| s.into_owned())
            .unwrap_or_else(|| de.appid.clone());
        let comment = de
            .comment(&locales)
            .map(|s| s.into_owned())
            .unwrap_or_default();
        entries.push(Entry {
            id: de.appid.clone(),
            name,
            comment,
            exec,
            terminal: de.terminal(),
            icon: de.icon().unwrap_or_default().to_string(),
            path: de.path.to_string_lossy().into_owned(),
            shadows: String::new(),
        });
    }

    // De-dup by id in walk order *before* sorting: Iter walks the dirs
    // in APPS_SUBDIRS order, which is user-first, so the first
    // occurrence is the highest-priority copy. The winner remembers the
    // next copy it hid.
    let mut winners: HashMap<String, usize> = HashMap::new();
    let mut kept: Vec<Entry> = Vec::with_capacity(entries.len());
    for e in entries {
        match winners.get(&e.id) {
            Some(&i) => {
                if kept[i].shadows.is_empty() {
                    kept[i].shadows = e.path;
                }
            }
            None => {
                winners.insert(e.id.clone(), kept.len());
                kept.push(e);
            }
        }
    }
    let mut entries = kept;
    entries.sort_by(|a, b| {
        a.name
            .to_lowercase()
            .cmp(&b.name.to_lowercase())
            .then_with(|| a.id.cmp(&b.id))
    });
    entries
}

/// Desktop entry metadata useful outside the launcher.
#[derive(Clone, PartialEq, Eq)]
pub struct DesktopAppMetadata {
    pub desktop_id: String,
    pub name: String,
    pub icon_path: String,
}

/// Resolve a Wayland app_id / XWayland WM_CLASS against every installed
/// rootfs. Returns the first launchable desktop entry whose id is a
/// plausible match, with its icon resolved to a PNG path.
///
/// Matching happens first, resolution second: this runs on the
/// compositor thread (via `Compositor::resolve_cached_app_metadata`) on
/// first window map, so it resolves exactly the one winning icon rather
/// than every icon in every installed rootfs.
pub fn resolve_metadata_for_app_id(app_id: &str) -> Option<DesktopAppMetadata> {
    let query = app_id.trim();
    if query.is_empty() {
        return None;
    }
    for rootfs in installed_rootfs_dirs() {
        for entry in scan_entries(&rootfs, false) {
            if desktop_id_matches_app_id(&entry.id, query) {
                return Some(DesktopAppMetadata {
                    desktop_id: entry.id,
                    name: entry.name,
                    icon_path: IconResolver::new(&rootfs).resolve_string(&entry.icon),
                });
            }
        }
    }
    None
}


fn installed_rootfs_dirs() -> Vec<PathBuf> {
    let distros = Path::new(&crate::app_paths::get().distros_dir);
    let mut out = Vec::new();
    let Ok(read_dir) = std::fs::read_dir(distros) else {
        return out;
    };
    for entry in read_dir.flatten() {
        let rootfs = entry.path().join("rootfs");
        if rootfs.is_dir() {
            out.push(rootfs);
        }
    }
    out.sort();
    out
}

pub(crate) fn desktop_id_matches_app_id(desktop_id: &str, app_id: &str) -> bool {
    let id = normalize_desktop_id(desktop_id);
    let app = normalize_desktop_id(app_id);
    if id == app {
        return true;
    }

    let id_tail = id.rsplit('.').next().unwrap_or(&id);
    let app_tail = app.rsplit('.').next().unwrap_or(&app);
    id_tail == app || id == app_tail || id_tail == app_tail
}

fn normalize_desktop_id(value: &str) -> String {
    value
        .trim()
        .trim_end_matches(".desktop")
        .to_ascii_lowercase()
}

/// JSON-encode the scan result for the JNI boundary. Each element is an
/// object: `{id, name, comment, exec, terminal, iconPath, path, shadows}`.
/// Always returns a valid JSON array (empty `[]` if the rootfs has no
/// apps).
///
/// This is where icons get resolved for the launcher list — Kotlin calls
/// it on `Dispatchers.IO`, so the per-entry stat walk is off the
/// compositor and UI threads.
pub fn scan_json(rootfs: &Path) -> String {
    let entries = scan_entries(rootfs, true);
    let icons = IconResolver::new(rootfs);
    let arr: Vec<_> = entries
        .iter()
        .map(|e| {
            json!({
                "id": e.id,
                "name": e.name,
                "comment": e.comment,
                "exec": e.exec,
                "terminal": e.terminal,
                "iconPath": icons.resolve_string(&e.icon),
                "path": e.path,
                "shadows": e.shadows,
            })
        })
        .collect();
    icons.finish_scan(!arr.is_empty());
    serde_json::Value::Array(arr).to_string()
}

/// Resolve one `Icon=` value against [rootfs] the way [scan_json]
/// does: an absolute PNG path, or empty. For the editor's preview and
/// icon picker cells.
pub fn resolve_icon(rootfs: &Path, icon: &str) -> String {
    IconResolver::new(rootfs).resolve_string(icon)
}

/// Every icon name in [rootfs] as a JSON array of `{name, user}`,
/// sorted, one entry per name; `user` marks names with a copy in the
/// user base (`/root/.local/share/icons`, where imports go). Names
/// only: resolving thousands of icons up front would take seconds, so
/// the picker resolves its cells lazily via [resolve_icon].
pub fn list_icons_json(rootfs: &Path) -> String {
    let arr: Vec<_> = IconResolver::new(rootfs)
        .list()
        .into_iter()
        .map(|(name, user)| json!({ "name": name, "user": user }))
        .collect();
    serde_json::Value::Array(arr).to_string()
}

/// `Type=Application` and not `Hidden`; `launchable_only` (see
/// `scan_entries`) also requires not `NoDisplay`.
fn is_relevant(de: &DesktopEntry, launchable_only: bool) -> bool {
    de.type_() == Some("Application") && !de.hidden() && (!launchable_only || !de.no_display())
}

/// One icon theme, possibly spread over several [ICON_BASES], with its
/// icon directories listed once in search order. The listing lets the
/// walk skip absent size tiers without stat'ing every
/// `<size>/<context>/<name>.png` combination — Adwaita ships three size
/// dirs, not ten.
struct ThemeDir {
    /// Directories holding `<name>.<ext>` files, in search order.
    dirs: Vec<IconDir>,
    /// `symbolic/<context>` dirs, preferred contexts first. Kept out of
    /// [ThemeDir::dirs] so a symbolic glyph never beats a real icon.
    symbolic: Vec<IconDir>,
}

struct IconDir {
    path: PathBuf,
    /// Under the user base ([USER_ICON_BASE]).
    user: bool,
}

/// An icon source the walk visits, in resolver order.
enum Source<'a> {
    /// A theme or pixmaps dir holding `<name>.<ext>`.
    Plain(&'a Path, bool),
    /// A theme's `symbolic/<context>` dir, holding `<name>-symbolic.svg`.
    Symbolic(&'a Path, bool),
}

/// Resolves `Icon=` values against one rootfs. Holds the theme order
/// and the SVG cache so a whole scan pays for them once; build with
/// [IconResolver::new] and reuse it for every entry.
struct IconResolver {
    rootfs: PathBuf,
    themes: Vec<ThemeDir>,
    pixmaps: PathBuf,
    cache: Option<IconCache>,
}

impl IconResolver {
    fn new(rootfs: &Path) -> Self {
        // Canonicalize so resolved icon paths share a prefix with the
        // `.desktop` paths the scan reports: Kotlin hands us
        // `/data/user/0/<pkg>/…`, which is a symlink to
        // `/data/data/<pkg>/…`, and the entry walk reports the latter.
        let rootfs = rootfs.canonicalize().unwrap_or_else(|_| rootfs.to_path_buf());
        Self {
            themes: resolve_theme_order(&rootfs),
            pixmaps: rootfs.join("usr/share/pixmaps"),
            cache: IconCache::new(&rootfs),
            rootfs,
        }
    }

    /// [IconResolver::resolve] as the string the JNI boundary carries:
    /// an absolute on-device PNG path, or empty when nothing was
    /// findable (Kotlin then draws a fallback glyph).
    fn resolve_string(&self, icon: &str) -> String {
        self.resolve(icon)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    /// Find an absolute on-device *PNG* path for [icon], or None.
    ///
    /// Resolution rules:
    ///   1. Absolute `Icon=/foo/bar.png` → rooted at the rootfs prefix,
    ///      rejected if it escapes the rootfs (see [rooted_path]).
    ///   2. Bare name `Icon=firefox` → walked over the resolved theme
    ///      order, then `usr/share/pixmaps`.
    ///   3. `Icon=name.<ext>` with a known image extension → stripped to
    ///      a bare name, then rule 2.
    ///
    /// An SVG source is rasterized into the per-install icon cache and
    /// that PNG is returned instead; see [icon_cache].
    fn resolve(&self, icon: &str) -> Option<PathBuf> {
        let icon = icon.trim();
        if icon.is_empty() {
            return None;
        }
        if icon.starts_with('/') {
            let p = rooted_path(&self.rootfs, icon)?;
            return p.is_file().then(|| self.as_png(p)).flatten();
        }
        let stem = strip_known_ext(icon);
        // A source whose match fails to rasterize falls through to the
        // next one rather than losing the icon: the `.fail` marker means
        // the broken source is parsed once, not once per scan.
        let symbolic_name = match stem.strip_suffix("-symbolic") {
            // `Icon=foo-symbolic` is legal; don't ask for `-symbolic-symbolic`.
            Some(_) => stem.to_string(),
            None => format!("{stem}-symbolic"),
        };
        for source in self.sources() {
            let png = match source {
                Source::Plain(dir, _) => ICON_FILE_EXTS
                    .iter()
                    .map(|ext| dir.join(format!("{stem}.{ext}")))
                    .find(|p| p.is_file())
                    .and_then(|p| self.as_png(p)),
                Source::Symbolic(dir, _) => {
                    let p = dir.join(format!("{symbolic_name}.svg"));
                    if p.is_file() { self.symbolic_png(p) } else { None }
                }
            };
            if png.is_some() {
                return png;
            }
        }
        None
    }

    /// Every icon source, in resolution order: each theme's dirs, then
    /// `usr/share/pixmaps` (the legacy pre-theme location, still where a
    /// fair number of Debian packages put their only icon), then, last
    /// because it loses the app's colours, each theme's symbolic glyphs.
    fn sources(&self) -> impl Iterator<Item = Source<'_>> {
        let plain = self
            .themes
            .iter()
            .flat_map(|t| &t.dirs)
            .map(|d| Source::Plain(&d.path, d.user));
        let pixmaps = std::iter::once(Source::Plain(&self.pixmaps, false));
        let symbolic = self
            .themes
            .iter()
            .flat_map(|t| &t.symbolic)
            .map(|d| Source::Symbolic(&d.path, d.user));
        plain.chain(pixmaps).chain(symbolic)
    }

    /// Every name [IconResolver::resolve] can find, once each, sorted
    /// case-insensitively, with whether any copy lives in the user base.
    /// Walks the same [IconResolver::sources], so a listed name
    /// resolves to the file the launcher would use.
    fn list(&self) -> Vec<(String, bool)> {
        let mut names: std::collections::HashMap<String, bool> = Default::default();
        for source in self.sources() {
            let (dir, user, symbolic) = match source {
                Source::Plain(dir, user) => (dir, user, false),
                Source::Symbolic(dir, user) => (dir, user, true),
            };
            let Ok(rd) = std::fs::read_dir(dir) else { continue };
            for entry in rd.flatten() {
                let Ok(file) = entry.file_name().into_string() else { continue };
                let Some(name) = listed_name(&file, symbolic) else { continue };
                // Dirents are free, but a symlink (themes are full of
                // them) may dangle, and resolve would skip it.
                let is_file = match entry.file_type() {
                    Ok(t) if t.is_file() => true,
                    Ok(t) if t.is_symlink() => entry.path().is_file(),
                    _ => false,
                };
                if is_file {
                    *names.entry(name.to_string()).or_default() |= user;
                }
            }
        }
        let mut out: Vec<_> = names.into_iter().collect();
        out.sort_by(|a, b| a.0.to_lowercase().cmp(&b.0.to_lowercase()).then_with(|| a.0.cmp(&b.0)));
        out
    }

    /// End-of-scan bookkeeping for the launcher list: drop cache
    /// entries this scan didn't reference, and report render failures
    /// once. [saw_entries] is false for a rootfs that produced nothing
    /// — transiently unreadable, say — where pruning would wipe a cache
    /// that is still perfectly good.
    fn finish_scan(&self, saw_entries: bool) {
        let Some(cache) = self.cache.as_ref() else {
            return;
        };
        if saw_entries {
            cache.prune();
        }
        let failures = cache.failure_count();
        if failures > 0 {
            warn!("launcher: {failures} icon(s) failed to render");
        }
    }

    /// [src] itself when it's a PNG, else its cached rendering. None for
    /// anything we can't turn into a PNG (unknown extension, unreadable
    /// or malformed SVG, no cache dir).
    fn as_png(&self, src: PathBuf) -> Option<PathBuf> {
        match src.extension().and_then(|e| e.to_str()) {
            Some(e) if e.eq_ignore_ascii_case("png") => Some(src),
            Some(e) if e.eq_ignore_ascii_case("svg") || e.eq_ignore_ascii_case("svgz") => {
                self.cached(&src, false)
            }
            _ => None,
        }
    }

    /// [src] rendered as a light glyph on a dark tile.
    fn symbolic_png(&self, src: PathBuf) -> Option<PathBuf> {
        self.cached(&src, true)
    }

    fn cached(&self, src: &Path, symbolic: bool) -> Option<PathBuf> {
        let rel = src.strip_prefix(&self.rootfs).ok()?;
        self.cache.as_ref()?.png_for(src, rel, symbolic)
    }
}

impl ThemeDir {
    /// List theme [name]'s icon dirs across [bases] (absolute, in
    /// [ICON_BASES] order). Both layouts in the wild are read:
    /// `<size>/<context>` (hicolor, Adwaita) and `<context>/<size>`
    /// (breeze). Order: preferred [ICON_CONTEXTS] first, then every
    /// other context by name; within a context, by [size_rank]; then
    /// layout, then base, so the user base wins a tie.
    fn new(bases: &[PathBuf], name: &str) -> Self {
        // (context tier, context, size rank, layout, base, dir)
        let mut dirs: Vec<(usize, String, (u32, u32), u8, usize, PathBuf)> = Vec::new();
        let mut symbolic: Vec<(usize, String, usize, PathBuf)> = Vec::new();
        for (base, base_dir) in bases.iter().enumerate() {
            let root = base_dir.join(name);
            for top in read_subdir_names(&root) {
                let top_dir = root.join(&top);
                if top == "symbolic" {
                    for context in read_subdir_names(&top_dir) {
                        let dir = top_dir.join(&context);
                        symbolic.push((context_tier(&context), context, base, dir));
                    }
                } else if let Some(rank) = size_rank(&top) {
                    for context in read_subdir_names(&top_dir) {
                        let dir = top_dir.join(&context);
                        dirs.push((context_tier(&context), context, rank, 0, base, dir));
                    }
                } else {
                    for size in read_subdir_names(&top_dir) {
                        if let Some(rank) = size_rank(&size) {
                            let dir = top_dir.join(&size);
                            dirs.push((context_tier(&top), top.clone(), rank, 1, base, dir));
                        }
                    }
                }
            }
        }
        dirs.sort();
        symbolic.sort();
        let icon_dir = |base: usize, path: PathBuf| IconDir { path, user: base == USER_ICON_BASE };
        Self {
            dirs: dirs.into_iter().map(|(_, _, _, _, b, p)| icon_dir(b, p)).collect(),
            symbolic: symbolic.into_iter().map(|(_, _, b, p)| icon_dir(b, p)).collect(),
        }
    }
}

/// Sort tier of a context: its [ICON_CONTEXTS] index, or after them.
fn context_tier(context: &str) -> usize {
    ICON_CONTEXTS
        .iter()
        .position(|c| *c == context)
        .unwrap_or(ICON_CONTEXTS.len())
}

/// Sort key of a size dir name, or None if it isn't one. [ICON_SIZES]
/// entries (spelled `48x48` or bare `48`, `NxN` first) rank in list
/// order; any other `N`, `NxN` or `@scale` dir ranks after them,
/// largest effective size first.
fn size_rank(name: &str) -> Option<(u32, u32)> {
    if name == "scalable" {
        let i = ICON_SIZES.iter().position(|s| *s == "scalable")? as u32;
        return Some((0, i * 2));
    }
    let (size, scale) = match name.split_once('@') {
        Some((size, scale)) => (size, scale.trim_end_matches('x').parse::<u32>().ok()?),
        None => (name, 1),
    };
    let (n, square) = match size.split_once('x') {
        Some((w, h)) if w == h => (w, true),
        Some(_) => return None,
        None => (size, false),
    };
    let n: u32 = n.parse().ok()?;
    if scale == 1 {
        if let Some(i) = ICON_SIZES.iter().position(|s| *s == n.to_string()) {
            return Some((0, i as u32 * 2 + u32::from(!square)));
        }
    }
    Some((1, u32::MAX - n.saturating_mul(scale)))
}

/// The icon name a theme/pixmaps file is listed under, or None for a
/// file [IconResolver::resolve] would never return: wrong extension,
/// or a stem that `resolve` would itself rewrite. Symbolic dirs list
/// `foo-symbolic.svg` as `foo`, the name that finds it.
fn listed_name(file: &str, symbolic: bool) -> Option<&str> {
    let (stem, ext) = file.rsplit_once('.')?;
    let stem = if symbolic {
        (ext == "svg").then_some(())?;
        stem.strip_suffix("-symbolic")?
    } else {
        ICON_FILE_EXTS.contains(&ext).then_some(())?;
        stem
    };
    let ok = !stem.is_empty() && stem.trim() == stem && strip_known_ext(stem) == stem;
    ok.then_some(stem)
}

/// Themes to search under [rootfs]'s [ICON_BASES], in order:
/// [ICON_THEMES] seeds expanded through their `index.theme` `Inherits=`
/// chains (breadth-first, de-duplicated, missing themes dropped), with
/// `hicolor` forced last as the spec's universal fallback. A theme
/// exists if any base has it; `index.theme` comes from the first base
/// that does.
fn resolve_theme_order(rootfs: &Path) -> Vec<ThemeDir> {
    let bases: Vec<PathBuf> = ICON_BASES
        .iter()
        .map(|b| rootfs.join(b))
        .filter(|b| b.is_dir())
        .collect();
    let exists = |name: &str| bases.iter().any(|b| b.join(name).is_dir());
    let mut queue: VecDeque<String> = ICON_THEMES.iter().map(|t| t.to_string()).collect();
    let mut seen: HashSet<String> = HashSet::new();
    let mut names: Vec<String> = Vec::new();
    while let Some(name) = queue.pop_front() {
        if !seen.insert(name.clone()) || !exists(&name) {
            continue;
        }
        if let Some(base) = bases.iter().find(|b| b.join(&name).join("index.theme").is_file()) {
            queue.extend(theme_inherits(&base.join(&name)));
        }
        names.push(name);
    }
    // hicolor is the fallback of last resort, so an inherited parent
    // (Adwaita's `AdwaitaLegacy`, breeze's `breeze-dark`, …) that got
    // appended behind it still gets searched first.
    names.retain(|n| n != "hicolor");
    if exists("hicolor") {
        names.push("hicolor".to_string());
    }
    names.iter().map(|name| ThemeDir::new(&bases, name)).collect()
}

/// Immediate subdirectory names of [dir]; empty if it can't be read.
/// Symlinked size dirs count (`default` is often a symlink farm), so
/// this follows links rather than trusting the dirent type.
fn read_subdir_names(dir: &Path) -> Vec<String> {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    rd.flatten()
        .filter(|e| e.path().is_dir())
        .filter_map(|e| e.file_name().into_string().ok())
        .collect()
}

/// Parent themes from `<theme_dir>/index.theme`'s `Inherits=` key.
/// Minimal line scan rather than a real ini parse: `Inherits` is only
/// valid in the leading `[Icon Theme]` group, so we stop at the next
/// group header — which also keeps us out of the hundreds of
/// per-directory groups that make these files big.
fn theme_inherits(theme_dir: &Path) -> Vec<String> {
    let Ok(file) = std::fs::File::open(theme_dir.join("index.theme")) else {
        return Vec::new();
    };
    let mut groups = 0;
    for line in BufReader::new(file).lines().map_while(Result::ok) {
        let line = line.trim();
        if line.starts_with('[') {
            groups += 1;
            if groups > 1 {
                break;
            }
            continue;
        }
        if let Some(value) = line.strip_prefix("Inherits") {
            if let Some(value) = value.trim_start().strip_prefix('=') {
                return value
                    .split(',')
                    .map(|t| t.trim().to_string())
                    .filter(|t| !t.is_empty())
                    .collect();
            }
        }
    }
    Vec::new()
}

/// Re-root an absolute guest path under [rootfs], lexically resolving
/// `.` / `..` first. None if the path climbs out of the rootfs —
/// `Icon=` is guest-controlled, and the launcher has no business
/// reading (let alone parsing) anything outside the install.
fn rooted_path(rootfs: &Path, abs: &str) -> Option<PathBuf> {
    let mut rel = PathBuf::new();
    for comp in Path::new(abs).components() {
        match comp {
            Component::Normal(c) => rel.push(c),
            Component::ParentDir => {
                if !rel.pop() {
                    return None;
                }
            }
            Component::RootDir | Component::CurDir | Component::Prefix(_) => {}
        }
    }
    Some(rootfs.join(rel))
}


/// Strip a `.<known image ext>` suffix (case-insensitive) from [name].
/// `Icon=org.gnome.Files` keeps the dotted appid intact (no stripping —
/// `Files` isn't a known extension); `Icon=firefox.png` becomes `firefox`.
fn strip_known_ext(name: &str) -> &str {
    let lower = name.to_ascii_lowercase();
    for ext in KNOWN_ICON_EXTS {
        if let Some(stem) = lower.strip_suffix(ext).and_then(|s| s.strip_suffix('.')) {
            return &name[..stem.len()];
        }
    }
    name
}

/// Drop `%f`, `%u`, `%F`, `%U`, `%i`, `%c`, `%k`, `%d`, `%D`, `%n`, `%N`,
/// `%v`, `%m` from an Exec line. We don't substitute since the launcher
/// never has URIs / icons / file lists to pass. `%%` collapses to a
/// literal `%`. Trailing whitespace from a stripped trailing field code
/// is trimmed.
fn strip_field_codes(exec: &str) -> String {
    let mut out = String::with_capacity(exec.len());
    let mut chars = exec.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '%' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('%') => out.push('%'),
            Some(_) => {} // drop %X
            None => out.push('%'),
        }
    }
    out.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// Locales we want translated `Name=` / `Comment=` for. Best-effort —
/// reads `LANG` / `LC_MESSAGES` from the host process env (the
/// CompositorService starts the JVM, so LANG is what Android set).
/// Empty list = use the default (untranslated) value only.
fn current_locales() -> Vec<String> {
    let mut out = Vec::new();
    for var in &["LC_ALL", "LC_MESSAGES", "LANG"] {
        if let Ok(v) = std::env::var(var) {
            // Strip ".UTF-8" suffix etc. — DesktopEntry::name handles
            // both `en_US` and `en` prefixes via its own fallback.
            let trimmed = v.split('.').next().unwrap_or("").to_string();
            if !trimmed.is_empty() && !out.contains(&trimmed) {
                out.push(trimmed);
            }
            break;
        }
    }
    out
}
