# Chroot extraction: root tar with no in-app traversal defense

`Archive.runTarScript` (chroot install path) runs toybox
`tar -xf <file> -C <dest>` via `Su.run` — as real root — with no
rejection of `../` members, absolute paths, or
symlink-then-write-through-symlink sequences. Containment rests
entirely on toybox tar's own semantics, which we neither control nor
document. Contrast the release-path extractor `ProotArchiveExtractor`
(tawcroot + proot), which does this properly: per-entry
canonicalize-and-contain, hardlink both-endpoint checks, pre-delete
before file writes.

Not currently exploitable: every shipping bootstrap is
integrity-verified before extraction and chroot is a debug-only
method. But if chroot is ever promoted, or ever combined with a
`BootstrapVerification.None` distro, this becomes "hostile tarball as
uid 0".

Fix ideas: route chroot extraction through `ProotArchiveExtractor`
(then chown/chmod via su afterwards), or add a containment pre-scan of
the archive listing before handing it to root tar.

Found in the 2026-07 production-readiness sweep.
