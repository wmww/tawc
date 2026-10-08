//! SVG → PNG cache for launcher icons.
//!
//! Android's `BitmapFactory` decodes PNG but not SVG, and most of a
//! modern distro's app icons are SVG only. Rather than teach all three
//! Kotlin decoders a second format, `launcher.rs` rasterizes SVG
//! sources here and hands Kotlin a PNG path like any other — the JNI
//! contract stays "`iconPath` is a decodable PNG, or empty".
//!
//! Cached files live in `<distros>/<id>/icon-cache/`, a sibling of the
//! rootfs: app-owned (so uninstall removes them, and keys can't collide
//! across installs) and reachable without `app_paths`, which
//! `nativeLauncherScan` may run before.
//!
//! Symbolic icons (`<name>-symbolic.svg`) are a special case: they are
//! a single near-black colour and would vanish on a dark background, and
//! a cached PNG can't follow the app theme. They are rendered as a light
//! glyph on a dark rounded tile, matching `ic_app_fallback`.
//!
//! The input is guest-controlled, so rendering is fenced: oversized
//! sources are skipped, parse+render runs under `catch_unwind`, and a
//! source that fails once leaves a `.fail` marker so the next scan
//! doesn't re-parse it.

use std::cell::RefCell;
use std::collections::HashSet;
use std::ffi::OsString;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};

/// Edge of the rendered square. Covers the launcher row (~56 dp at 3×),
/// the recents task icon and the 2/3-safe-zone pin bitmap (324 px
/// canvas → 216 px of content) without storing anything app icons have
/// the detail to use.
const RENDER_PX: u32 = 192;

/// Bumped when the rendering changes in a way that should invalidate
/// every existing cache entry. Part of the key, so old files simply
/// stop being referenced and get pruned.
const CACHE_FORMAT_VERSION: u32 = 1;

/// Symbolic glyph size as a fraction of the tile, about the margin the
/// fallback icons draw their glyphs at.
const SYMBOLIC_GLYPH_SCALE: f32 = 0.6;

/// Inset and corner radius of the symbolic backing tile, as fractions
/// of [RENDER_PX]. Same proportions as `ic_app_fallback`'s rounded
/// black square, so a themed symbolic icon and the fallback glyphs read
/// as the same family.
const SYMBOLIC_TILE_INSET: f32 = 1.0 / 24.0;
const SYMBOLIC_TILE_RADIUS: f32 = 3.5 / 24.0;

/// Backing tile and glyph colours for symbolic icons. Symbolic SVGs are
/// a single near-black colour, so they'd vanish on a dark background and
/// a cached PNG can't follow the app theme; baking light-on-dark is the
/// one choice that works either way.
const SYMBOLIC_TILE_RGBA: [u8; 4] = [0, 0, 0, 255];
const SYMBOLIC_GLYPH_RGB: [u8; 3] = [255, 255, 255];

/// Sources above this are not app icons — they're map data or a
/// decompression bomb. Skipped rather than rendered.
const MAX_SOURCE_BYTES: u64 = 1024 * 1024;

/// Leaked `.tmp` files (process killed mid-render) are only pruned once
/// they're old enough that no live scan could still be writing them.
const TMP_GRACE_SECS: u64 = 60;

/// Rasterizes SVG icons into `<distros>/<id>/icon-cache/`, tracking
/// which entries a scan touched so [IconCache::prune] can drop the rest.
pub struct IconCache {
    dir: PathBuf,
    /// Cache filenames referenced by this scan, for pruning.
    referenced: RefCell<HashSet<OsString>>,
    /// Sources that failed to render this scan; reported once, not
    /// once per icon.
    failures: RefCell<u32>,
}

impl IconCache {
    /// Cache beside [rootfs]. None when the rootfs has no parent (it
    /// always does in practice — `<distros>/<id>/rootfs`); SVG icons
    /// then simply don't resolve.
    pub fn new(rootfs: &Path) -> Option<Self> {
        let dir = rootfs.parent()?.join("icon-cache");
        Some(Self {
            dir,
            referenced: RefCell::new(HashSet::new()),
            failures: RefCell::new(0),
        })
    }

    /// Path to a PNG rendering of [src], rendering it if the cache
    /// doesn't already hold one. None if the source is unreadable,
    /// oversized, malformed, or previously failed.
    ///
    /// [rel] is the rootfs-relative source path; it goes into the key so
    /// two installs' identically-sized icons can't alias, and so a
    /// package upgrade (new mtime/len) produces a new file.
    ///
    /// [symbolic] renders the source as a monochrome glyph on a neutral
    /// tile instead of as-is; see [render_symbolic].
    pub fn png_for(&self, src: &Path, rel: &Path, symbolic: bool) -> Option<PathBuf> {
        let meta = std::fs::metadata(src).ok()?;
        if !meta.is_file() || meta.len() > MAX_SOURCE_BYTES {
            return None;
        }
        let key = cache_key(rel, &meta, symbolic);

        let png = self.dir.join(format!("{key}.png"));
        self.referenced.borrow_mut().insert(png.file_name()?.into());
        if png.is_file() {
            return Some(png);
        }
        let fail = self.dir.join(format!("{key}.fail"));
        self.referenced.borrow_mut().insert(fail.file_name()?.into());
        if fail.exists() {
            return None;
        }

        if std::fs::create_dir_all(&self.dir).is_err() {
            return None;
        }
        match render_to(src, &png, symbolic) {
            Some(()) => Some(png),
            None => {
                *self.failures.borrow_mut() += 1;
                // Zero-length marker: a bad SVG is parsed once, not on
                // every scan for the life of the install.
                let _ = std::fs::write(&fail, b"");
                None
            }
        }
    }

    /// Delete cache files this scan didn't reference — icons whose
    /// source changed or whose app was uninstalled. Call only from a
    /// scan that saw the whole rootfs; a partial scan would evict live
    /// entries (they'd be re-rendered, but for nothing).
    pub fn prune(&self) {
        let Ok(rd) = std::fs::read_dir(&self.dir) else {
            return;
        };
        let referenced = self.referenced.borrow();
        for entry in rd.flatten() {
            let name = entry.file_name();
            if referenced.contains(&name) {
                continue;
            }
            let path = entry.path();
            match path.extension().and_then(|e| e.to_str()) {
                Some("png") | Some("fail") => {
                    let _ = std::fs::remove_file(&path);
                }
                // A concurrent scan (launcher + shortcut trampoline) may
                // be mid-write, so only sweep tmp files old enough that
                // nobody can still own them.
                Some("tmp") if older_than(&entry, TMP_GRACE_SECS) => {
                    let _ = std::fs::remove_file(&path);
                }
                _ => {}
            }
        }
    }

    /// Number of sources that failed to render this scan. Reported once
    /// per scan — per-icon logging would be a flood on a broken theme.
    pub fn failure_count(&self) -> u32 {
        *self.failures.borrow()
    }
}

/// Stable-per-input name for a cached rendering. Not a cryptographic
/// hash: the inputs aren't adversarial in the collision sense (a guest
/// that wants a wrong icon can just ship a wrong icon), and every
/// unreferenced file is pruned anyway.
fn cache_key(rel: &Path, meta: &std::fs::Metadata, symbolic: bool) -> String {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    rel.hash(&mut hasher);
    symbolic.hash(&mut hasher);
    meta.len().hash(&mut hasher);
    meta.modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0)
        .hash(&mut hasher);
    RENDER_PX.hash(&mut hasher);
    CACHE_FORMAT_VERSION.hash(&mut hasher);
    format!("{:016x}", hasher.finish())
}

/// Render [src] to a [RENDER_PX]² transparent PNG at [dst]: aspect
/// preserved, centred, never upscaled past the square. Written to a
/// per-call temp file and renamed, so a concurrent render of the same
/// key (another scan, or picker cells on several threads) never sees or
/// clobbers a half-written PNG. None on any failure.
fn render_to(src: &Path, dst: &Path, symbolic: bool) -> Option<()> {
    let data = std::fs::read(src).ok()?;
    // usvg parses guest-supplied XML; a panic there would take the
    // whole scan (and, on the metadata path, the compositor) down.
    let pixmap = std::panic::catch_unwind(|| {
        if symbolic {
            render_symbolic(&data)
        } else {
            rasterize(&data, RENDER_PX, 1.0)
        }
    })
    .ok()
    .flatten()?;
    save_atomic(&pixmap, dst)
}

/// Rasterize the SVG at [src] to a [px]² PNG at [dst], for icons the
/// app imports into its own store (`launcher/icons/`). Same fences as
/// the cache: size cap, `catch_unwind`, temp file + rename.
pub fn rasterize_file(src: &Path, dst: &Path, px: u32) -> bool {
    let Ok(meta) = std::fs::metadata(src) else { return false };
    if !meta.is_file() || meta.len() > MAX_SOURCE_BYTES || px == 0 {
        return false;
    }
    let Ok(data) = std::fs::read(src) else { return false };
    std::panic::catch_unwind(|| rasterize(&data, px, 1.0))
        .ok()
        .flatten()
        .and_then(|p| save_atomic(&p, dst))
        .is_some()
}

fn save_atomic(pixmap: &resvg::tiny_skia::Pixmap, dst: &Path) -> Option<()> {
    static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let seq = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let tmp = dst.with_extension(format!("{}-{seq}.tmp", std::process::id()));
    if pixmap.save_png(&tmp).is_err() {
        let _ = std::fs::remove_file(&tmp);
        return None;
    }
    match std::fs::rename(&tmp, dst) {
        Ok(()) => Some(()),
        Err(_) => {
            let _ = std::fs::remove_file(&tmp);
            None
        }
    }
}

/// Render [data] centred in a [px]² transparent pixmap, aspect
/// preserved, with the longest side at [fill] of the square.
fn rasterize(data: &[u8], px: u32, fill: f32) -> Option<resvg::tiny_skia::Pixmap> {
    let tree = resvg::usvg::Tree::from_data(data, &resvg::usvg::Options::default()).ok()?;
    let size = tree.size();
    let longest = size.width().max(size.height());
    if !(longest.is_finite() && longest > 0.0) {
        return None;
    }
    let scale = px as f32 * fill / longest;
    let mut pixmap = resvg::tiny_skia::Pixmap::new(px, px)?;
    let transform = resvg::tiny_skia::Transform::from_translate(
        (px as f32 - size.width() * scale) / 2.0,
        (px as f32 - size.height() * scale) / 2.0,
    )
    .pre_scale(scale, scale);
    resvg::render(&tree, transform, &mut pixmap.as_mut());
    Some(pixmap)
}

/// A symbolic SVG as a light glyph on a dark rounded tile.
///
/// Recolouring by rewriting the fill in the SVG source is fragile (the
/// colour can come from `fill`, `style`, a class, or a `use` reference),
/// so we render normally and keep only the alpha: whatever the source
/// drew becomes a mask, and the mask is painted in the foreground
/// colour over the tile.
fn render_symbolic(data: &[u8]) -> Option<resvg::tiny_skia::Pixmap> {
    let mut glyph = rasterize(data, RENDER_PX, SYMBOLIC_GLYPH_SCALE)?;
    let [r, g, b] = SYMBOLIC_GLYPH_RGB;
    for px in glyph.pixels_mut() {
        let a = px.alpha();
        // Premultiplied, so each channel is scaled by the coverage.
        let scale = |c: u8| ((c as u32 * a as u32 + 127) / 255) as u8;
        *px = resvg::tiny_skia::PremultipliedColorU8::from_rgba(scale(r), scale(g), scale(b), a)?;
    }

    let mut out = resvg::tiny_skia::Pixmap::new(RENDER_PX, RENDER_PX)?;
    let [tr, tg, tb, ta] = SYMBOLIC_TILE_RGBA;
    let mut paint = resvg::tiny_skia::Paint::default();
    paint.set_color(resvg::tiny_skia::Color::from_rgba8(tr, tg, tb, ta));
    paint.anti_alias = true;
    out.fill_path(
        &rounded_square(
            RENDER_PX as f32 * SYMBOLIC_TILE_INSET,
            RENDER_PX as f32 * (1.0 - SYMBOLIC_TILE_INSET),
            RENDER_PX as f32 * SYMBOLIC_TILE_RADIUS,
        )?,
        &paint,
        resvg::tiny_skia::FillRule::Winding,
        resvg::tiny_skia::Transform::identity(),
        None,
    );
    out.draw_pixmap(
        0,
        0,
        glyph.as_ref(),
        &resvg::tiny_skia::PixmapPaint::default(),
        resvg::tiny_skia::Transform::identity(),
        None,
    );
    Some(out)
}

/// Axis-aligned rounded square from `lo` to `hi` with corner radius `r`,
/// corners as cubic arcs (the usual 0.5523 circle approximation).
fn rounded_square(lo: f32, hi: f32, r: f32) -> Option<resvg::tiny_skia::Path> {
    const KAPPA: f32 = 0.5523;
    let k = r * KAPPA;
    let mut pb = resvg::tiny_skia::PathBuilder::new();
    pb.move_to(lo + r, lo);
    pb.line_to(hi - r, lo);
    pb.cubic_to(hi - r + k, lo, hi, lo + r - k, hi, lo + r);
    pb.line_to(hi, hi - r);
    pb.cubic_to(hi, hi - r + k, hi - r + k, hi, hi - r, hi);
    pb.line_to(lo + r, hi);
    pb.cubic_to(lo + r - k, hi, lo, hi - r + k, lo, hi - r);
    pb.line_to(lo, lo + r);
    pb.cubic_to(lo, lo + r - k, lo + r - k, lo, lo + r, lo);
    pb.close();
    pb.finish()
}

fn older_than(entry: &std::fs::DirEntry, secs: u64) -> bool {
    entry
        .metadata()
        .and_then(|m| m.modified())
        .and_then(|t| t.elapsed().map_err(|e| std::io::Error::other(e.to_string())))
        .map(|age| age.as_secs() >= secs)
        .unwrap_or(false)
}
