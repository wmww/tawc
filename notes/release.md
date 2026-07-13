# Release Process

Releases are signed APKs published as GitHub release assets. No app-store distribution yet.

## Versioning

- Plain release counter: versions are `1`, `2`, `3`, … No semver — the app has no breaking/non-breaking distinction for users; every release is expected to migrate existing installs forward.
- `versionName` in `app/build.gradle.kts` is the single source of truth; `versionCode = versionName.toInt()`, so Android's monotonic-versionCode upgrade requirement is satisfied automatically.
- Each release commit is tagged `vN` (annotated).

## Prep steps (agent)

When asked to prep a release:

1. Bump `versionName` in `app/build.gradle.kts` to the next integer.
2. Draft release notes from `git log <last-tag>..` (first release: summarize the feature set instead). Keep them user-facing: features, fixes, known limitations.
3. Commit as `release: vN`, then tag `vN` (the prep request counts as the explicit ask to commit/tag; do not push).
4. Hand off: print the human steps below with the concrete version filled in, plus the drafted notes (e.g. as a `--notes-file` in scratch or inline for copy/paste).

The release build cannot be run by the agent: the signing keystore lives in a different user account where Claude does not run.

## Publish steps (human, as the key-owning user)

1. `scripts/build-release-apk.sh` — builds `assembleRelease` (default graphics `libhybris,cpu`), zipaligns, signs, verifies, and renames the artifact to `app/build/outputs/apk/release/tawc-vN.apk` (version read from the APK via aapt2).
2. Smoke-test that exact APK on the physical phone: fresh install + launch + distro install + run an app (e.g. lxterminal); for later releases also install *over* the previous release to catch signing/versionCode upgrade breakage. The release build differs from the dev loop (no debug methods, production graphics set), so dev-loop testing does not cover it.
3. Push `main` and the tag.
4. `gh release create vN tawc-vN.apk --title "tawc vN" --notes-file <notes>`.

## Debuggability over size

Release builds are deliberately NOT minified, obfuscated, or stripped (release block + `packaging.jniLibs.keepDebugSymbols` in `app/build.gradle.kts`): user-reported Java stack traces are readable as-is, and native tombstones come out of the device symbolized — no mapping.txt archiving, no unstripped-artifact hunting. This roughly doubles the APK (~29 vs ~13 MB R8-minified); anything under ~50 MB is an acceptable trade. `proguard-rules.pro` stays correct regardless, so minifying is a one-flag change if a size ceiling ever appears.

## Keystore

- The signing key is load-bearing: every release must be signed with the same key or users cannot upgrade without uninstalling — which deletes their app-private distro installs (real data loss).
- Keystore lives at `$KEYSTORE_PATH` (default `~/Android/keystore.jks`) in the key-owning user's account. Keep it and its password backed up somewhere durable.

## Release notes

No in-repo `CHANGELOG.md`; GitHub release notes are the changelog. Revisit if another distribution channel appears.
