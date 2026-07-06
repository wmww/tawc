# tawcroot — build & installer integration

## Build integration

A `tawcroot/build.sh` script alongside `scripts/build-proot.sh`:

- Cross-compile with the NDK's clang for `arm64-v8a` and `x86_64`.
- Pure C11 + a couple of `.S` files for production sources. No
  talloc, no autoconf, no config.h.
- **Statically linked, non-PIE, freestanding**
  (`-static -nostdlib -nostartfiles -no-pie -ffreestanding`).
  Required by both the re-exec architecture (no bionic linker
  before our `_start`) and the seccomp filter's IP-based stub
  allowlisting (stable address across re-execs — see §"Why non-PIE").
  Handler/runtime objects also use `-fno-stack-protector` and no
  sanitizer/profiling instrumentation so guest glibc and
  tawcroot's runtime cannot collide through compiler-inserted
  helper paths. No `dlopen`, no Android property/IPC features
  (we don't use these). Expect ~30 KB binary today; ~1–2 MB once
  the manual ELF loader and full handler land.
- **Fixed base addresses** per arch via `-Wl,--image-base=`:
  0x2000000000 (aarch64), 0x40000000 (x86_64). NDK lld rejects
  `-Ttext-segment` outright; use `--image-base`. The x86_64 base
  is constrained by static-bionic PLT32 weak-undef relocations —
  see §"Why non-PIE" for the full story. The aarch64 base mirrors
  proot's `LOADER_ADDRESS` convention.
- Output `build/tawcroot-<abi>/libtawcroot.so` + strip; testhost
  variant at `build/tawcroot-<abi>/libtawcroot-testhost.so` or
  `build/tawcroot-host/tawcroot-testhost`.
- Gradle `packTawcroot` task copies the production binary into
  `app/src/main/jniLibs/<abi>/libtawcroot.so`. Testhost is
  never copied — it's only used by the cleat orchestrator on host
  and by `tawcroot/test.sh --device` via adb push.
- Clones cleat into `./deps/cleat/` at a pinned commit (gitignored,
  same pattern as `deps/libhybris/`, `deps/libxkbcommon/`, `deps/proot/`) on
  first `--abi=host` run. Pin lives in the build script; bumping
  it is a deliberate change. cleat brings its own vendored STC —
  we do *not* clone STC separately. **cleat is built into the
  test orchestrator only**, not into either tawcroot binary.
- Builds the cleat orchestrator (`build/tawcroot-host/tests`)
  natively with the host toolchain (hosted glibc binary). Also
  cross-builds it for `aarch64`/`x86_64` against bionic when
  `--tests` is passed, landing at `build/tawcroot-<abi>/tests`. The
  Android variant is what `tawcroot/test.sh --device` runs on the
  device — same source set, same filter syntax, same exit code
  semantics as the host build (cleat is plain POSIX C + vendored
  STC; no glibc-only deps). See §"Testing strategy".
- **Host build is incremental** via `tawcroot/Makefile`:
  `gcc -MMD -MP` for header dep tracking, `-j$(nproc)` for
  parallel compile. Warm rebuilds (no source changes) take ~30 ms;
  touching one production source rebuilds + relinks both tawcroot
  binaries in ~300 ms; touching a cleat header rebuilds just the
  test files that include it. For tight inner loops you can call
  `make -C tawcroot` directly — `tawcroot/build.sh --abi=host`
  adds the cleat-clone step on top. Cross-ABI NDK builds stay in
  the bash flow (NDK setup is bash-shaped, and they're not in
  the inner loop).

This keeps it consistent with the existing proot build (proot
ships as `libproot.so` + `libproot-loader.so`; tawcroot ships as
`libtawcroot.so` for production, with `tawcroot-testhost` and
`tests` as host-only test artifacts).

### Source list lives in two places

The production `.c` set is duplicated between `tawcroot/build.sh`
(`SRC_C_PROD`, used for the NDK cross-builds and on-device tests)
and `tawcroot/Makefile` (`PROD_C`, used for the host build that
`tawcroot/test.sh --host` exercises). **Adding a new `.c` file means
editing both.**

The split exists for a reason — the host Makefile uses gcc with
header-dep tracking for fast incremental builds; the cross-build
needs NDK-flavoured bash that the Makefile would clutter — but it's
a correctness trap. The chroot.c regression was exactly this: it
was added to `PROD_C` but missed in `SRC_C_PROD`, so `tawcroot/test.sh
--host` passed (host build was complete) while the device shipped a
binary that didn't even include the chroot handler. (There would
have been a linker error if the cross-build had run, but Gradle's
`buildTawcroot` task didn't list source dirs as inputs and skipped
the rebuild — so the stale pre-fix binary stayed staged in jniLibs.)

Mitigations: Gradle's `buildTawcroot$abi` now lists `tawcroot/src`
and `tawcroot/include` as inputs (so source-only edits invalidate
the cache), and the cross-build *does* fail loudly on link if a
referenced symbol is missing — once it actually runs. If the two
lists drift again, `tawcroot/test.sh --device` is the canonical way to
catch it: running the on-device tests forces a cross-build for the
target ABI and any link error surfaces immediately.

## Installer integration

A new `TawcrootMethod.kt` next to `ProotMethod.kt`. Same shape:

- Build the argv (bind table + chroot exec) in `startInside`.
- Apply the same bootstrap-cache + tar-extract pipeline as
  `ProotMethod`.
- No `/dev/shm` host bind — the SIGSYS handler emulates POSIX shm
  via `memfd_create` (`src/shm.c`). See §"Bootstrap & entry".
- Run `TawcInstaller.installInto` to copy libhybris (and the glvnd
  vendor JSON) into the rootfs as real files at `/usr/lib/hybris/`.

The Kotlin `InstallationMethod` enum already has an `extra` slot
(`metadata.json`); we add `TAWCROOT` as a value and the radio in
`InstallActivity` defaults to it on rootless devices once we're
confident.

`scripts/rootfs-run.sh` reads `metadata.json` to decide between
chroot/proot/tawcroot (today it's just chroot/proot). One more
case in the `case method` switch.

