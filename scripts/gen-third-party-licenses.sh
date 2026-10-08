#!/bin/bash
# Regenerate app/src/main/assets/licenses.json — the data behind the
# in-app Settings > About > Licenses screen.
#
# Sources, all read from the working tree (no network):
#   - LICENSE / LICENSE.MIT      TAWC's own terms and the GPLv3 text
#   - deps/**/{LICENSE,COPYING}* vendored native + Java source licenses
#   - cargo metadata             compositor crates, texts from the
#                                local ~/.cargo registry checkout
#   - GRADLE_LICENSES below      curated map for Maven artifacts, whose
#                                licenses live in POMs rather than files
#   - licenses/                  texts with no in-tree source at all
#
# Output shape: components are grouped by license *family* (MIT,
# Apache-2.0, ...) so the app can show a ~12-row index instead of one
# endless page. Within a family, texts are deduplicated by body with the
# copyright lines lifted out, which collapses the ~80 MIT variants that
# differ only in their copyright holder down to a handful of bodies.
#
# Hard-wrapped prose is reflowed into single paragraphs so Android can
# wrap it to the screen; without that, upstream's 70-column wrapping and
# the phone's narrower column produce a ragged double-wrap. Blocks that
# don't look like prose (lists, headers, ASCII layout) are preserved
# verbatim and rendered monospace.
#
# Requires the dep checkouts (scripts/ensure-deps.sh) and a populated
# cargo registry. The generated file is checked in, so ordinary builds
# never run this.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$ROOT_DIR/app/src/main/assets/licenses.json"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

command -v cargo >/dev/null || { echo "ERROR: cargo not found" >&2; exit 1; }
[ -d "$ROOT_DIR/deps/xwayland-src" ] || {
    echo "ERROR: deps/ not populated; run scripts/ensure-deps.sh first" >&2
    exit 1
}
# libmd is a tarball dep, only extracted by build-xwayland.sh; without it
# its notice would silently drop out.
[ -d "$ROOT_DIR/deps/xwayland-src/libmd" ] || {
    echo "ERROR: deps/xwayland-src/libmd missing; run scripts/build-xwayland.sh first" >&2
    exit 1
}

echo "=== Resolving Maven artifacts (releaseRuntimeClasspath) ==="
GRADLE_LIST="$ROOT_DIR/app/build/third-party-licenses/gradle-deps.txt"
mkdir -p "$(dirname "$GRADLE_LIST")"
( cd "$ROOT_DIR" && ./gradlew -q :app:dependencies --configuration releaseRuntimeClasspath ) \
    | grep -oE "[a-zA-Z0-9._-]+:[a-zA-Z0-9._-]+:[0-9][a-zA-Z0-9._-]*" \
    | sort -u >"$GRADLE_LIST"
[ -s "$GRADLE_LIST" ] || { echo "ERROR: no Maven artifacts resolved" >&2; exit 1; }
echo "    $(wc -l <"$GRADLE_LIST") artifacts"

echo "=== Resolving compositor crates ==="
CARGO_META="$(mktemp)"
trap 'rm -f "$CARGO_META"' EXIT
cargo metadata --manifest-path "$ROOT_DIR/compositor/Cargo.toml" \
    --format-version 1 --filter-platform aarch64-linux-android >"$CARGO_META"

echo "=== Writing $OUT ==="
ROOT_DIR="$ROOT_DIR" CARGO_META="$CARGO_META" OUT="$OUT" python3 - <<'PY'
import hashlib
import json
import os
import pathlib
import re

ROOT = pathlib.Path(os.environ["ROOT_DIR"])
OUT = pathlib.Path(os.environ["OUT"])

SOURCE_URL = "https://github.com/wmww/tawc"

# Native/vendored components whose code, headers, or data end up in the
# APK. Value is the checkout dir; license files are globbed out of it.
NATIVE = {
    "libhybris (TAWC fork)": "deps/libhybris",
    "libxkbcommon": "deps/libxkbcommon",
    "smithay (TAWC fork)": "deps/smithay",
    "cleat": "deps/cleat",
    "OpenSSH (sftp-server)": "deps/openssh-portable",
    "termux-app (terminal-emulator, terminal-view, termux-shared extra-keys)": "deps/termux-app",
}
for d in sorted((ROOT / "deps/xwayland-src").iterdir()):
    if d.is_dir():
        NATIVE[d.name] = f"deps/xwayland-src/{d.name}"

# Components with no license file in-tree. Text is taken from the named
# sibling checkout, which carries an identical upstream license.
CURATED_NATIVE = {
    "libdrm": ("MIT", "deps/xwayland-src/libx11/COPYING"),
    "android-headers (Halium)": ("Apache-2.0", "deps/libhybris/LICENSE.Apache2"),
    # Data compiled into the remote access crate (remote/src).
    "RFC 1751 dictionary": ("RFC 1751", "licenses/rfc1751.txt"),
    # Terminal font, checked in as app/src/main/res/font/hack_regular.ttf.
    "Hack font v3.003": ("MIT, Bitstream Vera", "licenses/hack.txt"),
}

# Maven artifacts: licenses live in POM metadata, not files in the tree.
# Matched longest-prefix-first against "group:artifact".
GRADLE_LICENSES = [
    ("com.github.luben:zstd-jni", "BSD-2-Clause"),
    ("org.bouncycastle", "Bouncy Castle Licence"),
    ("org.tukaani:xz", "0BSD"),
    ("androidx.", "Apache-2.0"),
    ("com.google.", "Apache-2.0"),
    ("org.jetbrains", "Apache-2.0"),
    ("org.apache.commons", "Apache-2.0"),
    ("commons-codec", "Apache-2.0"),
    ("commons-io", "Apache-2.0"),
]

# Where each Maven license's verbatim text comes from. Texts that exist
# only in a POM or on a project website are checked in under licenses/;
# see licenses/README.md.
GRADLE_LICENSE_TEXTS = {
    "Apache-2.0": "deps/libhybris/LICENSE.Apache2",
    "BSD-2-Clause": "licenses/zstd-jni.txt",
    "Bouncy Castle Licence": "licenses/bouncycastle.txt",
    "0BSD": "licenses/xz-java.txt",
}

LICENSE_GLOBS = ("LICENSE*", "COPYING*", "NOTICE*", "LICENCE*")
# Build tooling / CI license files that don't cover shipped code.
SKIP_LICENSE_FILES = {"LICENSE-BUILDTOOLS"}


def read_text(p):
    try:
        return p.read_text(encoding="utf-8", errors="replace").strip("\n")
    except OSError:
        return None


def license_files(dirpath):
    """Verbatim license texts directly inside a checkout root."""
    out = []
    d = ROOT / dirpath
    if not d.is_dir():
        return out
    for pat in LICENSE_GLOBS:
        for f in sorted(d.glob(pat)):
            if not f.is_file() or f.name in SKIP_LICENSE_FILES:
                continue
            t = read_text(f)
            if t:
                out.append(t)
    return out


def dedup_key(text):
    """Group identical licenses that differ only in whitespace.

    Upstream copies of the same license vary in indentation and line
    wrapping, which would otherwise scatter one Apache-2.0 into a dozen
    entries. Texts are never rewritten for grouping — only the key is
    normalized, and what gets displayed stays verbatim.
    """
    return hashlib.sha256(" ".join(text.split()).encode()).hexdigest()


FAMILIES = [
    (r"GNU GENERAL PUBLIC LICENSE.*Version 3", "GNU GPL v3"),
    (r"GNU LESSER GENERAL PUBLIC LICENSE.*Version 2\.1", "GNU LGPL v2.1"),
    (r"GNU GENERAL PUBLIC LICENSE.*Version 2", "GNU GPL v2"),
    (r"Mozilla Public License.*2\.0", "Mozilla Public License 2.0"),
    (r"Apache License.*Version 2\.0", "Apache License 2.0"),
    (r"The FreeType Project LICENSE", "FreeType License"),
    (r"UNICODE.*LICEN[CS]E", "Unicode License"),
    (r"Permission is hereby granted, free of charge", "MIT"),
    (r"Permission to use, copy, modify, and/or distribute", "ISC / 0BSD"),
    (r"Permission to use, copy, modify, distribute", "MIT-style (X11)"),
    (r"3\.\s*Neither the name", "BSD 3-Clause"),
    (r"Redistribution and use in source and binary forms", "BSD 2-Clause"),
    (r"altered from any source distribution", "zlib"),
]


def classify(body):
    flat = " ".join(body.split())
    for pattern, name in FAMILIES:
        if re.search(pattern, flat, re.I | re.S):
            return name
    return "Other licenses"


PROSE_MIN_LINE = 55


def reflow(body):
    """Split a license body into renderable blocks.

    Hard-wrapped prose becomes one long paragraph the app can rewrap;
    anything else (lists, headers, ASCII layout) is preserved verbatim
    and rendered monospace. The signature of hard-wrapped prose is that
    every line but the last is near-full-width.
    """
    blocks, current = [], []

    def flush():
        if not current:
            return
        lines = list(current)
        current.clear()
        stripped = [l.strip() for l in lines]
        # A real list has several marker lines. One line happening to
        # start with "(1)" mid-sentence — as the GPL preamble does — is
        # still hard-wrapped prose, and forcing it to monospace is what
        # produces the ragged double-wrap this reflow exists to avoid.
        markers = sum(
            1 for l in lines if re.match(r"^\s*([-*•]|\(?[a-z0-9]{1,3}[.)])\s", l)
        )
        prose = (
            len(lines) > 1
            and all(len(l) >= PROSE_MIN_LINE for l in stripped[:-1])
            and markers < 2
        )
        if prose:
            blocks.append({"pre": False, "text": " ".join(stripped)})
        else:
            blocks.append({"pre": True, "text": "\n".join(lines)})

    for line in body.split("\n"):
        if line.strip():
            current.append(line)
        else:
            flush()
    flush()
    return blocks


# component label -> list of verbatim license texts
components = {}


def add(label, texts):
    if texts:
        components.setdefault(label, []).extend(texts)


missing_native = []
for label, path in NATIVE.items():
    texts = license_files(path)
    if not texts:
        missing_native.append(f"{label} ({path})")
    add(label, texts)

# A checkout that grows or loses its license file must not silently drop
# out of the attribution list — add it to CURATED_NATIVE instead.
unexplained = [m for m in missing_native if m.split(" (")[0] not in CURATED_NATIVE]
if unexplained:
    raise SystemExit(
        "ERROR: no license file found for vendored component(s); add a "
        "CURATED_NATIVE entry:\n  " + "\n  ".join(sorted(unexplained))
    )

for label, (spdx, src) in CURATED_NATIVE.items():
    t = read_text(ROOT / src)
    if t:
        add(f"{label} ({spdx})", [t])

# --- Rust crates -----------------------------------------------------
meta = json.load(open(os.environ["CARGO_META"]))
pkgs = {p["id"]: p for p in meta["packages"]}
nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
root_id = meta["resolve"]["root"]

seen, stack = set(), [root_id]
while stack:
    cur = stack.pop()
    if cur in seen:
        continue
    seen.add(cur)
    for dep in nodes.get(cur, {}).get("deps", []):
        kinds = {dk.get("kind") for dk in dep.get("dep_kinds", [])}
        if kinds and kinds <= {"dev"}:  # dev-deps are not shipped
            continue
        stack.append(dep["pkg"])
seen.discard(root_id)

declared_only = []
for pid in sorted(seen, key=lambda i: (pkgs[i]["name"], pkgs[i]["version"])):
    p = pkgs[pid]
    label = f'{p["name"]} {p["version"]} ({p.get("license") or "see source"})'
    src_dir = pathlib.Path(p["manifest_path"]).parent
    texts = []
    for pat in LICENSE_GLOBS:
        for f in sorted(src_dir.glob(pat)):
            if f.is_file() and f.name not in SKIP_LICENSE_FILES:
                t = read_text(f)
                if t:
                    texts.append(t)
    if texts:
        add(label, texts)
    else:
        declared_only.append(label)

# --- Maven artifacts -------------------------------------------------
unmapped_maven = []
for line in (ROOT / "app/build/third-party-licenses/gradle-deps.txt").read_text().splitlines():
    coord = line.strip()
    if not coord:
        continue
    ga = ":".join(coord.split(":")[:2])
    spdx = next((lic for pre, lic in GRADLE_LICENSES if ga.startswith(pre)), None)
    if spdx is None:
        unmapped_maven.append(coord)
        continue
    text = read_text(ROOT / GRADLE_LICENSE_TEXTS[spdx])
    if not text:
        raise SystemExit(f"ERROR: missing license text for Maven license {spdx!r}")
    add(f"{coord} ({spdx})", [text])

if unmapped_maven:
    raise SystemExit(
        "ERROR: no license mapping for Maven artifact(s); add them to "
        "GRADLE_LICENSES:\n  " + "\n  ".join(sorted(unmapped_maven))
    )

# --- Group: family -> distinct text -> components --------------------
families = {}
for label, texts in components.items():
    for t in texts:
        fam = families.setdefault(classify(t), {})
        entry = fam.setdefault(dedup_key(t), {"text": t, "components": set()})
        entry["components"].add(label)

sections = []


def section(title, subtitle, entries):
    sections.append({"title": title, "subtitle": subtitle, "entries": entries})


section(
    "TAWC's own code",
    "MIT",
    [{"components": [], "blocks": reflow(read_text(ROOT / "LICENSE.MIT") or "")}],
)
section(
    "This app as a whole",
    "GNU GPL v3",
    [{"components": [], "blocks": reflow(read_text(ROOT / "LICENSE") or "")}],
)

for fam, texts in sorted(
    families.items(),
    key=lambda kv: (-sum(len(e["components"]) for e in kv[1].values()), kv[0]),
):
    entries = [
        {
            "components": sorted(e["components"], key=str.lower),
            "blocks": reflow(e["text"]),
        }
        for e in sorted(texts.values(), key=lambda e: -len(e["components"]))
    ]
    total = sum(len(e["components"]) for e in entries)
    section(fam, f"{total} component{'s' if total != 1 else ''}", entries)

if declared_only:
    section(
        "Declared without a bundled text",
        f"{len(declared_only)} components",
        [
            {
                "components": sorted(declared_only, key=str.lower),
                "blocks": reflow(
                    "These crates declare their license in package metadata but "
                    "ship no license file. The declared identifier applies, and the "
                    "full text of each named license appears elsewhere in this list."
                ),
            }
        ],
    )

doc = {
    "intro": [
        "TAWC's own source code is MIT licensed. The app also bundles the "
        "extra-keys widget from termux-shared, which is GPLv3-only, so the "
        "distributed app as a whole is conveyed under the GNU General Public "
        "License version 3.",
        "Complete corresponding source for this build, and the source of every "
        "component listed here, is available at:",
    ],
    "sourceUrl": SOURCE_URL,
    "sections": sections,
}

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(json.dumps(doc, separators=(",", ":"), ensure_ascii=False), encoding="utf-8")

print(f"components: {len(components)}  sections: {len(sections)}")
for s in sections:
    print(f"    {s['title']} — {s['subtitle']} ({len(s['entries'])} text(s))")
print(f"bytes: {OUT.stat().st_size}")
PY

# The flat text file this replaced is no longer read by anything.
rm -f "$ROOT_DIR/app/src/main/assets/third-party-licenses.txt"

echo "Wrote $OUT"
