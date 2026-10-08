# Licensing

## Position

TAWC's own source is **MIT** (`LICENSE.MIT`). The **distributed APK is
GPL-3.0-only** (`LICENSE`), because it links GPLv3 code. Both statements
are true at once and neither replaces the other:

- Anyone reusing TAWC's *sources* gets them under MIT.
- Anyone receiving a *built APK* receives it under GPLv3.

`LICENSE` holds the GPLv3 text rather than the MIT text so GitHub's
repo-level license detection reports the terms the binary actually ships
under. The README's Licensing section states the split.

## What makes the binary GPLv3

`termux-extrakeys` compiles `com/termux/shared/termux/extrakeys/**` out
of the vendored termux-app checkout. termux-app is GPLv3-only; its
Apache-2.0 exception covers `terminal-emulator` and `terminal-view`
(which descend from jackpal's Android-Terminal-Emulator) but **not**
`termux-shared`. So the extra-keys row — and only it — pulls the whole
distributed APK under GPLv3. See `termux-extrakeys/build.gradle.kts` and
[terminal.md](terminal.md).

This is a deliberate trade, not an accident: the alternative is
reimplementing the extra-keys toolbar.

## What is *not* in the release APK

`proot` is **GPLv2-only**, which is genuinely incompatible with GPLv3.
It stays out of release builds by construction — proot is a debug-only
install method, so `lib/arm64-v8a/` in a release APK contains
`libtawcroot.so` and no proot. If proot ever became release-supported,
this incompatibility would have to be resolved first.

libhybris ships a mixed license set (Apache-2.0, BSD variants, ISC, MIT,
LGPLv2.1, GPL3). LGPLv2.1 §3 permits use under GPLv2-or-later, so it
composes with GPLv3 here.

Everything else that ships — 148 compositor crates, the Xwayland stack,
121 Maven artifacts — is permissive (MIT/Apache-2.0/BSD/ISC/0BSD/Zlib),
plus one MPL-2.0 crate (`freedesktop-desktop-entry`, weak per-file
copyleft, GPL-compatible) and FreeType under the FTL. The crate count
jumped with `resvg` (launcher SVG icons): 29 crates, all
MIT/Apache-2.0/BSD/Zlib. Recent `resvg` is `MIT OR Apache-2.0`; releases
before 0.45 were MPL-2.0, so check the license again on a version bump.
Remote access (`remote/`, [remote-access.md](remote-access.md)) added
~100 more (russh, rustls/ring, tokio-tungstenite, …), also permissive:
`ring` is `Apache-2.0 AND ISC`, `webpki-roots` (Mozilla's CA list) is
`CDLA-Permissive-2.0`. It also compiles in RFC 1751's dictionary (ids and secrets),
`licenses/rfc1751.txt`. Its `sftp-server` is OpenSSH (BSD-style
`LICENCE`, from `deps/openssh-portable`).

## How the obligations are met

| Obligation | Where |
|---|---|
| Convey the GPLv3 text with the program | `LICENSE`, and in-app under Settings > About |
| Offer corresponding source | public repo, tagged per release |
| Retain permissive notices in binary distributions | in-app licenses screen |

The in-app screens are `LicensesActivity` (an index of license
families) and `LicenseSectionActivity` (one family's components and
texts), backed by the checked-in asset
`app/src/main/assets/licenses.json`. That file is generated — see
[building.md](building.md) "Third-party license text" for how to
regenerate it and what it reads.

## When adding a dependency

Re-run `scripts/gen-third-party-licenses.sh` and commit the regenerated
asset. The script errors out on a Maven coordinate with no entry in its
`GRADLE_LICENSES` table, and on a vendored checkout with no license
file, so an unclassified dependency fails the regeneration instead of
quietly vanishing from the attribution list.

Watch for anything **GPLv2-only**, **AGPL**, or proprietary: the first
two cannot be combined with the GPLv3 the APK is under, and would need
resolving before shipping rather than after.
