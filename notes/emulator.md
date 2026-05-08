# Android emulator support

The compositor can run against an Android Studio AVD as well as a real
device. Useful for iterating on non-GPU work (Wayland protocol logic,
input, text-input/IME, window management) without needing the phone
plugged in. GPU/AHB/libhybris work still has to run on a real device.

## What works
- `adb` / `su` / chroot (Magisk grants `su` to adb shell, same SELinux
  context `u:r:magisk:s0` as a real device).
- Installing / launching the compositor APK; compositor reaches the
  event loop. (Smithay didn't support gfxstream's missing
  `EGL_KHR_surfaceless_context` until our fork; see "Smithay patches"
  in `notes/architecture.md` or the `tawc-patches` branch.)
- The Arch chroot (x86_64 build).

## Known limitations
- libhybris won't run on the emulator. The APK ships libhybris only for
  `arm64-v8a`; on x86_64 emulator builds the asset is absent and
  `LibhybrisLinker` no-ops. Anything that exercises the AHB import path
  (most integration tests) will still fail; SHM-only paths
  (magenta-tinted) should work. The blocker is **not** missing GPU
  vendor blobs (the emulator does ship `libEGL_emulation.so`,
  `vulkan.ranchu.so`, gralloc/mapper) — see "libhybris on x86_64" below.
- Architecture is x86_64 (real device is aarch64). Most code doesn't
  care, but anything arch-specific won't transfer.

## libhybris on x86_64

The notes used to claim libhybris failed on the emulator due to "no
Android GPU vendor blobs to bind to". That was wrong. Verified
2026-05-04: `/vendor/lib64/egl/libEGL_emulation.so` (gfxstream),
`/vendor/lib64/hw/vulkan.ranchu.so`, `/vendor/lib64/hw/gralloc.default.so`,
`/vendor/lib64/hw/mapper.ranchu.so` all exist on a stock
`google_apis;x86_64` AVD — the same surface a normal Android app sees.

We cross-built libhybris for x86_64 (native gcc, `--enable-arch=x86-64`)
and exercised it inside the Arch chroot. The build script isn't
checked in — it was a one-shot to surface the real blocker. To
re-create: copy `client/build-libhybris-aarch64`, drop the
`HOST_TRIPLE` cross-prefix, swap the configure flag to
`--enable-arch=x86-64`, and skip the `vulkan` subdir (see #1 below).

Three issues, in order:

1. **Vulkan plugin won't assemble.** Our fork's `vulkan.c` replaces
   upstream's IFUNC-based `VULKAN_IDLOAD` with hand-written aarch64
   trampolines (`adrp` / `ldr` / `br x16`). x86_64 needs an
   `__x86_64__` branch with `mov rax, [rip+sym]; jmp rax`. Tractable
   but unnecessary while #3 is fatal.

2. **The rest builds clean** for `--enable-arch=x86-64`: common,
   q-linker, EGL, GLES1/2, gralloc, hwc2, ui, hardware, platforms.
   The q-linker's TLSDESC resolver (`tlsdesc_resolver.S`) is gated on
   `WANT_ARCH_ARM64` already; not needed on x86_64 (different
   relocation model).

3. **Stock x86_64 bionic crashes on the first call into framework
   EGL.** With the libs pushed into the chroot, the q-linker loaded
   the entire dependency tree successfully (`/system/lib64/libEGL.so`,
   `/apex/com.android.runtime/lib64/bionic/{libc,libm,libdl}.so`,
   libcutils/libutils/libhidlbase/binder_ndk/graphics.mapper@4.0/…).
   The first `eglGetDisplay()` call segfaulted at `mov rax, [rax+0x820]`
   with `RAX=NULL` — a bionic TCB→bionic_tls slot dereference
   returning NULL on a glibc thread.

   **Why:** on a glibc thread `%fs:0` points at glibc's `tcbhead_t`,
   not bionic's. Bionic's slot 9 (`TLS_SLOT_BIONIC_TLS`) and the
   `bionic_tls` struct it should point to don't exist there. Same
   *class* of bug as the aarch64 bionic-on-stock-Android `TPIDR_EL0`
   issue our fork fixes; the aarch64 fix doesn't transfer.

   On aarch64 the entire bionic codebase reads TLS through one
   fixed-width instruction (`MRS Xn, TPIDR_EL0`). Our TLS thunk
   patcher walks `.text` 4 bytes at a time, masks each word, swaps
   matches in-place for a branch to a thunk that loads from a global
   we control. ~200 lines of code, no disassembler needed, and
   instruction width never changes so no surrounding code shifts.

### What it'd take on x86_64

Three design options, none cheap. Each has a real failure mode worth
naming up front so future work doesn't repeat the same dead ends.

**(A) Co-locate one shared TCB at one `%fs`.** Lay out a single block
that satisfies both glibc's `tcbhead_t` (self/dtv/sysinfo/stack_guard
at the offsets glibc reads) and bionic's slot table (slot 0
self-pointer, slot 5 stack_guard, slot 9 → our calloc'd `bionic_tls`,
slot 8 → minimal DTV). On thread creation, retrofit `%fs` once via
`arch_prctl(ARCH_SET_FS, …)`. Inlined `%fs:offset` reads on both
sides then "just work" — same `%fs`, both sides find what they
expect.

The catch: this only works if bionic's writes to its slots don't
corrupt glibc invariants at the same byte offsets (and vice versa).
Initial desk-check looked rosy because slot 0 (self) and slot 5
(stack_guard at 0x28) line up trivially, but the audit isn't done.
Real risks I noted on closer reading:
- **Slot 5 / `stack_guard` at 0x28.** Both sides read it for
  `-fstack-protector`. They generate independent canaries; whichever
  side wrote last wins, the other's prologues `__stack_chk_fail` on
  return. Have to pick one canary at retrofit time and trust nothing
  rewrites it.
- **Slot 6 / `pointer_guard` at 0x30.** glibc uses it to mangle
  pointers in `setjmp`/`atexit`/etc.; bionic uses slot 6 for
  sanitizer-thread-local. Bionic writing slot 6 corrupts every
  subsequent glibc `longjmp`. Likely incompatible.
- **glibc DTV at 0x08 vs bionic THREAD_ID at slot 1 (also 0x08).**
  Genuine offset collision. Probably benign because bionic mostly
  writes THREAD_ID and uses it as a tag, but not proven.
- **Beyond the header.** Both sides spread state past the
  well-known offsets; bionic's slot table reserves more in newer
  Android versions.

The right next step before betting work on (A) is an `objdump -d |
grep '%fs:'` audit across all loaded `.so`s on both sides — bucket
by offset, cross-reference against `bionic_asm_tls.h` and glibc's
`tls.h`, look for offsets where both sides write. If the offsets
cooperate, (A) is the smallest design (low hundreds of lines of new
hooks.c, mostly bookkeeping). If they don't, (A) is unsound and you
fall back to (C).

**(B) Per-call `%fs` swap (trampoline at function boundary).**
Wrap libhybris's exposed surface (libEGL entry points, vendor dlsym
targets); on entry `arch_prctl(ARCH_SET_FS, bionic_tcb)`, on return
swap back. **Doesn't actually work** — and this is the failure mode
worth remembering, because it sounds reasonable until you stop and
think about inlining. Bionic's TLS reads aren't at function
boundaries you can intercept; they're inlined into callers via:
- `errno` (expands to `&__get_thread()->errno_value` → `mov %fs:8`
  inlined into every libc syscall failure path)
- `__stack_chk_guard` (compiler emits `mov %fs:0x28` directly in
  every stack-protected prologue/epilogue)
- `pthread_self()` (inline `mov %fs:8`)
- `thread_local` in bionic-built libc++
- `__get_tls()` / `__get_thread()` private headers, used widely

So inlined `%fs:offset` reads inside vendor `libEGL_emulation.so`
have no symbol boundary to wrap. (B) misses essentially all of them.
Don't pitch this approach — it's a trap.

**(C) Patch every `%fs:offset` read in `.text`.** The aarch64
strategy carried to x86. Math is fine — bionic always knows the slot
at compile time, so the displacement is a constant immediate; adding
a constant K to push bionic's slots into a non-glibc region is a
straight rewrite. The hard parts are mechanical:
- **Variable instruction length.** `mov %fs:0x48, %rax` is 9 bytes;
  `%fs:0x48, %r11` is 10 (REX); `add %fs:0x48, %rax` is 9; `cmpq $0,
  %fs:0x48` is 10; `incq %fs:0x48` is 8. Word-by-word scanning won't
  find them — you need a real x86 disassembler (Capstone, Zydis,
  XED-style hand-roll). The aarch64 patcher doesn't need one.
- **Can't grow instructions in place.** Shifting an 8-bit
  displacement to 32-bit grows the instruction by 3 bytes; every
  subsequent instruction shifts; relative jumps/calls break;
  `.eh_frame` and symbol offsets break. Workaround: detour-style
  trampoline. Overwrite the matched instruction with a 5-byte
  relative `jmp` (plus pad) into a generated stub that does the real
  work with the shifted offset and `jmp`s back. Each occurrence
  needs its own stub (different destination register, different
  return site); the stub arena has to be within ±2GB of `.text` so
  the relative jump can reach. This is small-JIT territory —
  basically a Frida/Detours-shaped piece of new code.
- **Telling instructions from data.** `.text` has jump tables,
  alignment padding, switch tables, literal pools. Linear-sweep
  disassembly desyncs the moment it hits any of these. Need to seed
  from function entries via symbol table / `.eh_frame` and walk
  CFGs.
- **Sequencing.** Patching has to happen at module load before
  constructors run (libhybris's aarch64 patcher already does this;
  the hook point exists). On x86 you also have to handle the case
  where `dlopen`-loaded modules pull in further bionic libraries
  whose constructors then run — patcher hook needs to fire on every
  load, recursively.
- **Address-takes.** Rare bionic patterns like
  `&__stack_chk_guard` pass the slot address as a value; once it's
  in a register you can't tell it from any other pointer. Patcher
  can't fix these.

(C) handles inlines correctly because it operates at instruction
level; that's its big advantage over (B). Cost is the
disassembler+CFG-walker+detour-allocator infrastructure, plus the
debugging time when a misclassified data byte gets "patched" and the
program JITs garbage with no obvious cause.

**Net.** (A) is the smallest if the offset audit comes back clean
and there's no future Android version that breaks the layout. (C) is
the only sound option if the audit fails, and it's strictly more
code but has no soundness assumption to verify. (B) is a tempting
trap; don't.

Until somebody picks one up, libhybris stays aarch64-only and the
emulator stays SHM-only for GPU.

## One-time setup

### 1. Android SDK + emulator + system image
The host needs the cmdline-tools, platform-tools, emulator and an x86_64
Google APIs (not Google Play — Play images don't allow `adb root`)
system image. The repo does not bundle these. Quickest route:

    mkdir -p ~/Android/Sdk && cd ~/Android/Sdk
    curl -sSLo cmdline-tools.zip \
        https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q cmdline-tools.zip && mkdir -p cmdline-tools/latest
    mv cmdline-tools/{bin,lib,NOTICE.txt,source.properties} cmdline-tools/latest/
    rm cmdline-tools.zip
    export ANDROID_HOME=~/Android/Sdk
    export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
    export JAVA_HOME=/usr/lib/jvm/java-21-openjdk
    yes | sdkmanager --licenses >/dev/null
    sdkmanager 'platform-tools' 'emulator' 'platforms;android-36' \
        'system-images;android-36;google_apis;x86_64'

(`platform-tools` will shadow Arch's system `adb`. Either remove it
from PATH or rely on the system one — both versions work the same
against the AVD.)

### 2. AVD
    echo no | avdmanager create avd -n tawc-rooted \
        -k 'system-images;android-36;google_apis;x86_64' -d 'pixel_5'

### 3. First boot (no Magisk yet)
    emulator -avd tawc-rooted -no-window -no-audio -no-boot-anim -no-snapshot &
    # wait for boot
    until adb -s emulator-5554 shell getprop sys.boot_completed | grep -q 1; do sleep 5; done

(Headless `-no-window` is fine for the rootAVD step. Day-to-day, use the
`scripts/emulator.sh` helper — see "Day-to-day" below.)

### 4. Root with Magisk via rootAVD (per-AVD-scoped)
The rootAVD checkout lives at `./deps/rootAVD/` (gitignored, like `./deps/libhybris`).
Clone if missing:

    git clone --depth 1 https://gitlab.com/newbit/rootAVD.git ./deps/rootAVD

> **Do not patch the shared system-image ramdisk.**
> rootAVD's documented form,
> `rootAVD.sh system-images/<api>/<vendor>/<arch>/ramdisk.img`,
> overwrites the SDK's per-system-image ramdisk in place. Every
> AVD created from that image — including `tawc-rootless` — then
> boots a Magisk-init'd ramdisk and silently auto-installs the
> Magisk APK on first boot, leaking root semantics into every
> "stock" AVD. The agentic setup hit exactly this once; symptom
> is a "Magisk: Upgrade to full Magisk" dialog popping on the
> rootless AVD's home screen.

Patch a per-AVD copy instead. rootAVD treats its argv as
`$ANDROID_HOME`-relative, so we point its `ANDROID_HOME` at
`$HOME` for the patch step and pass an AVD-local path:

    # Stage a copy of the pristine ramdisk into the AVD dir.
    cp "$ANDROID_HOME/system-images/android-36/google_apis/x86_64/ramdisk.img" \
       "$HOME/.android/avd/tawc-rooted.avd/ramdisk.img"

    # Tell the emulator to use the AVD-local ramdisk on launch.
    printf '\nramdisk.path=%s/.android/avd/tawc-rooted.avd/ramdisk.img\n' \
        "$HOME" >> "$HOME/.android/avd/tawc-rooted.avd/config.ini"

    # Patch only the AVD-local copy. rootAVD writes its
    # `ramdisk.img.backup` next to the patched file (in the AVD
    # dir, *not* the system image dir).
    ANDROID_HOME="$HOME" ANDROID_SERIAL=emulator-5554 \
        bash ./deps/rootAVD/rootAVD.sh \
            .android/avd/tawc-rooted.avd/ramdisk.img

This patches the AVD-local `ramdisk.img` with Magisk init,
installs the Magisk APK on the running emulator, and shuts the
emulator down. Cold-boot it again:

    emulator -avd tawc-rooted -no-window -no-audio -no-boot-anim -no-snapshot &

If you ever discover the system-image ramdisk got patched anyway
(check `ls $ANDROID_HOME/system-images/android-36/google_apis/x86_64/ramdisk.img.backup`
— rootAVD only creates that when it patched in place), restore
the pristine copy with
`cp ramdisk.img.backup ramdisk.img` in that directory and wipe
any contaminated rootless AVD's userdata before next boot.

### 5. Pre-grant `su` to adb shell
On a real phone you'd tap "Grant" in the Magisk app. Headless equivalent
(only needs to be done once, persists across reboots):

    adb root                                 # userdebug images allow this
    adb shell 'magisk --sqlite "INSERT OR REPLACE INTO policies \
        (uid,policy,until,logging,notification) VALUES(2000,2,0,1,0);"'
    adb unroot

After this, `adb shell 'su -c id'` returns `uid=0(root) ... context=u:r:magisk:s0`,
matching a real Magisk-rooted device.

### 6. Bootstrap the chroot
    adb -s emulator-5554 install -r app/build/outputs/apk/debug/app-debug.apk
    TAWC_TARGET=emulator bash scripts/install-distro.sh arch tawcroot

This installs the tawc app and triggers its in-app installer, which
downloads the Arch x86_64 bootstrap tarball, extracts it to
`/data/data/me.phie.tawc/distros/arch/rootfs/`, configures pacman,
and installs `base-devel` + Wayland + GTK3 inside the chroot. Takes a
few minutes the first time. Idempotent — re-running skips done steps
(apart from a forced re-extract; uninstall + reinstall for a clean slate).

## Day-to-day

Two AVDs are supported:

- `tawc-rooted` (default) — Magisk-rooted via the rootAVD flow above.
  Required for the chroot install method (it needs `su` for bind-mounts
  and /dev/null setup), so this is the AVD all integration tests use.
- `tawc-rootless` (opt-in via `start rootless`) — stock AVD, no Magisk.
  Useful for testing the tawcroot/proot install methods on a non-rooted
  image. Won't render SHM client surfaces (no `setenforce 0`), and the
  chroot install method won't work.

To create the rootless AVD (one-time):

    echo no | avdmanager create avd -n tawc-rootless \
        -k 'system-images;android-36;google_apis;x86_64' -d 'pixel_5'

Skip steps 3–5 above for it — the rootless AVD just boots stock.
This AVD has no `ramdisk.path` line in its `config.ini`; it uses
the SDK's shared system-image ramdisk verbatim. As long as step 4
above has been done in the per-AVD-scoped form, the system-image
ramdisk stays pristine and the rootless AVD really is stock. If a
"Magisk: Upgrade to full Magisk" dialog ever appears here, the
shared ramdisk has been patched in place — see step 4's recovery
note.

Launch / shut down:

    bash scripts/emulator.sh start                 # current default (rooted)
    bash scripts/emulator.sh start rooted          # explicit rooted
    bash scripts/emulator.sh start rootless        # stock AVD
    bash scripts/emulator.sh start rooted --cold   # skip snapshot, full cold boot
    bash scripts/emulator.sh stop                  # stops every running tawc AVD
    bash scripts/emulator.sh stop rootless         # only stop the rootless one
    bash scripts/emulator.sh stop rooted           # only stop the rooted one

The variant is positional so callers (test scripts, CI, etc.) can name
exactly which AVD they want; if the default rotates later, scripts that
specify `rooted` keep getting rooted.

You can run both AVDs at once — the script resolves the right adb
serial via `adb emu avd name`, so `start rootless` always targets the
rootless one even if the rooted one is already on `emulator-5554`.
Bare `stop` with no variant stops whichever tawc AVDs are currently
running (both, if both are up).

`start` always runs the emulator windowed against an X display. There's
no headless mode — the window is the easiest way to see what's actually
happening. If `DISPLAY` is unset or points at an unreachable `:0`,
`start` auto-picks a usable X socket from `/tmp/.X11-unix/X*` owned by
the current uid (typically `:1` under an Xwayland-on-Wayland session)
and prints `==> using DISPLAY=:N`. The emulator's bundled Qt only ships
the xcb plugin, so a real X (or Xwayland) socket is required; if none
is reachable the script errors out instead of launching.

`stop` does `adb emu kill` against the AVD's actual serial so quickboot
saves cleanly, falls back to SIGTERM, and finally SIGKILLs after a 30s
grace period.

The script waits for `sys.boot_completed` then exits, leaving the
emulator running in the background. Logs go to `/tmp/emulator.log`
(override with `TAWC_EMULATOR_LOG`).

`-feature -QuickbootFileBacked` is passed unconditionally: with the
default file-backed Quickboot, qemu mmaps the 2 GB guest RAM onto
`snapshots/default_boot/ram.img`, so every guest memory store dirties a
page of that file and the host kernel flushes 50–150 MB/s to disk under
any meaningful guest activity (kswapd / zRAM churn especially). The
trade-off is a slower snapshot save on exit (it copies 2 GB instead of
relying on the file mapping); ongoing disk writes drop to ~zero.

Post-boot it also brings the AVD into a known-good state for tawc dev:

- `setenforce 0` — rootAVD's Magisk has no `magiskpolicy` binary, so
  the SELinux `type_transition` that lets the compositor mmap memfds
  from chroot clients can't be installed; permissive mode is the
  emulator-only workaround. Resets every reboot.
- If `me.phie.tawc` is installed, grants Magisk `su` to its uid (so
  `InstallationService` doesn't pop a prompt) and grants
  `POST_NOTIFICATIONS` (so the install foreground-service notification
  displays). Both grants reset on emulator wipe; the `su` policy
  survives normal reboots, the notification grant survives upgrades.
  These steps are no-ops when the APK isn't installed yet — install
  the APK then re-run the script to apply them.
- `settings put secure immersive_mode_confirmations confirmed` to
  suppress the fresh-AVD "swipe down to exit fullscreen" education
  popup, which otherwise eats the first taps tests send.
- `pm disable-user --user 0 com.google.android.inputmethod.latin` +
  `am force-stop` to disable Gboard. Its `StylusEducationPopupDialog`
  pops a "Try out your stylus" dialog on stylus-tool-type taps that
  covers the compositor and eats events. `ime disable` alone isn't
  enough — Gboard's package services keep running and pop the dialog
  even when it isn't the active IME; `pm disable-user` stops the
  whole package for user 0. Persists across reboots; resets on AVD
  wipe (so the script re-applies every `start`). tawc tests don't
  use Android's IME — Wayland clients have zwp_text_input;
  TEXT_INPUT broadcasts inject text directly into the compositor.
- Forces `hw.keyboard = yes` in the AVD's `config.ini`. `avdmanager`
  defaults this to `no`, which silently drops host-keyboard
  keystrokes in the emulator window (the soft keyboard works, but a
  physical keyboard doesn't). The patch is idempotent and applies on
  the next emulator launch — if the AVD is already running when the
  script flips the bit, restart it.

Windowed mode notes:
- The emulator's bundled Qt only ships an xcb (X11) plugin, no wayland
  plugin. On a Wayland desktop you need an Xwayland socket reachable as
  your user. The launcher auto-detects an `X*` socket in `/tmp/.X11-unix/`
  owned by you if `$DISPLAY` isn't already valid.
- On Sway specifically, run a nested sway as your user, or have your
  compositor expose Xwayland under a `DISPLAY` your shell can reach.
- If qemu reports "Could not find the Qt platform plugin 'wayland'" but
  also reports `Setting display: 0 configuration to: ...`, that's fine —
  the "(:0, )" in the warning is Qt's logger context, not the actual
  DISPLAY value.

Pick a target with `TAWC_TARGET=physical` or `TAWC_TARGET=emulator`,
or set the same value as one word in `./.tawctarget` for a standing
default. The host scripts (`run-integration-tests.sh`, …) source
`scripts/lib/select-device.sh`, which resolves the target and sets
`ANDROID_SERIAL` accordingly. Pre-setting `ANDROID_SERIAL` overrides
everything.

There is no auto-fallback: if `.tawctarget` (or `TAWC_TARGET`) is
missing/`none`, every host script errors out instead of silently
talking to the only thing connected. Likewise, if the requested kind
isn't connected (e.g. `.tawctarget=emulator` but no AVD running) the
script errors instead of substituting a real device. Bring the right
target up, or override `TAWC_TARGET` for that one command.

Run something in the chroot:

    TAWC_TARGET=emulator bash scripts/tawc-rootfs-run.sh "uname -m"

`tawc-rootfs-run` invokes `<installation-dir>/enter.sh` over `adb shell
su` and is the only host-side chroot helper. The mount + chroot script
lives in Kotlin (`ChrootMounter.enterScript`) and is rendered to disk
at install time, so emulator-vs-device differences (skip libhybris-only
mounts on emulator) are baked in once.

## SELinux on the emulator
On a real device, `ChrootMounter` uses `magiskpolicy --live` to install
a `type_transition` so that memfds the chroot's clients create get the
`appdomain_tmpfs` label and the compositor (running as `untrusted_app`)
can mmap them. rootAVD only patches the ramdisk — it never deploys the
full Magisk userspace, so `magiskpolicy` doesn't exist on the AVD. The
result: SHM client surfaces fail to render with an `avc: denied { write }`
on a `tmpfs:s0` memfd in logcat.

`scripts/emulator.sh start` works around this by running `setenforce 0`
once the AVD finishes booting. It's emulator-only, resets on reboot,
and we already gave up isolation by Magisk-rooting the AVD anyway. If
you boot the AVD by hand without `emulator.sh start`, run `adb shell
'su -c "setenforce 0"'` yourself.

## Implementation notes
- `scripts/lib/select-device.sh` — sourceable wrapper; resolves
  target from (in order) `ANDROID_SERIAL`, `TAWC_TARGET`,
  `./.tawctarget`, defaulting to `none`. Sets `ANDROID_SERIAL`
  on success; errors out on `none`, missing target type, or
  unknown value. No single-target auto-fallback.
- `scripts/tawc-rootfs-run.sh` — host-side chroot driver. Invokes
  `<installation-dir>/enter.sh` (auto-generated by `ChrootMounter`) over
  `adb shell su`.
- The emulator chroot uses minimal mounts (`dev`, `dev/pts`, `proc`,
  `sys`). It skips `apex`, `binderfs`, `vendor`, `system`,
  `system_ext`, `linkerconfig` since libhybris doesn't run there
  anyway.

## Magisk policy persistence
Once the policies row is in `/data/adb/magisk.db`, it survives
emulator reboots and snapshot save/load. If you wipe the AVD
(`emulator -avd tawc-rooted -wipe-data`), repeat steps 4–6.
