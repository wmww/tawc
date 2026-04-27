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
- libhybris won't run — the emulator has no Android GPU vendor blobs
  to bind to. `client/build-libhybris` refuses on emulator targets.
  Anything that exercises the AHB import path (most integration tests)
  will still fail; SHM-only paths (magenta-tinted) should work.
- Architecture is x86_64 (real device is aarch64). Most code doesn't
  care, but anything arch-specific won't transfer.

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
    echo no | avdmanager create avd -n tawc \
        -k 'system-images;android-36;google_apis;x86_64' -d 'pixel_5'

### 3. First boot (no Magisk yet)
    emulator -avd tawc -no-window -no-audio -no-boot-anim -no-snapshot &
    # wait for boot
    until adb -s emulator-5554 shell getprop sys.boot_completed | grep -q 1; do sleep 5; done

(Headless `-no-window` is fine for the rootAVD step. Day-to-day, use the
`client/start-emulator` helper — see "Day-to-day" below.)

### 4. Root with Magisk via rootAVD
The rootAVD checkout lives at `./rootAVD/` (gitignored, like `./libhybris`).
Clone if missing:

    git clone --depth 1 https://gitlab.com/newbit/rootAVD.git ./rootAVD

Then patch the AVD's ramdisk:

    ANDROID_SERIAL=emulator-5554 \
    bash ./rootAVD/rootAVD.sh \
        system-images/android-36/google_apis/x86_64/ramdisk.img
This patches the AVD's `ramdisk.img` with Magisk init, installs the
Magisk APK, and shuts the emulator down. Cold-boot it again:

    emulator -avd tawc -no-window -no-audio -no-boot-anim -no-snapshot &

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
    adb -s emulator-5554 install -r server/app/build/outputs/apk/debug/app-debug.apk
    adb -s emulator-5554 shell am start \
        -n me.phie.tawc/.install.ManageInstallationsActivity \
        --es autoAction install --es id arch
    adb -s emulator-5554 logcat -s tawc-install   # tail progress

This installs the tawc app and triggers its in-app installer, which
downloads the Arch x86_64 bootstrap tarball, extracts it to
`/data/data/me.phie.tawc/installations/arch/rootfs/`, configures pacman,
and installs `base-devel` + Wayland + GTK3 inside the chroot. Takes a
few minutes the first time. Idempotent — re-running skips done steps
(apart from a forced re-extract; uninstall + reinstall for a clean slate).

## Day-to-day

Launch the AVD:

    bash client/start-emulator           # windowed (default)
    bash client/start-emulator --headless
    bash client/start-emulator --cold    # skip snapshot, full cold boot

The script waits for `sys.boot_completed` then exits, leaving the
emulator running in the background. Logs go to `/tmp/emulator.log`
(override with `TAWC_EMULATOR_LOG`).

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

With both targets connected, set `TAWC_TARGET=device` or
`TAWC_TARGET=emulator`. The host scripts (`build-libhybris`,
`run-integration-tests.sh`, …) source `client/select-device.sh`, which
sets `ANDROID_SERIAL` accordingly. Pre-setting `ANDROID_SERIAL`
overrides everything. With only one target connected, `TAWC_TARGET`
isn't needed.

Run something in the chroot:

    TAWC_TARGET=emulator bash client/tawc-chroot-run "uname -m"

`tawc-chroot-run` invokes `<installation-dir>/enter.sh` over `adb shell
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

`client/start-emulator` works around this by running `setenforce 0` once
the AVD finishes booting. It's emulator-only, resets on reboot, and we
already gave up isolation by Magisk-rooting the AVD anyway. If you boot
the AVD by hand without `start-emulator`, run `adb shell 'su -c
"setenforce 0"'` yourself.

## Implementation notes
- `client/select-device.sh` — sourceable wrapper; sets `ANDROID_SERIAL`
  based on `TAWC_TARGET`, errors if ambiguous.
- `client/tawc-chroot-run` — host-side chroot driver. Invokes
  `<installation-dir>/enter.sh` (auto-generated by `ChrootMounter`) over
  `adb shell su`.
- `client/build-libhybris` — refuses to run against an emulator target.
- The emulator chroot uses minimal mounts (`dev`, `dev/pts`, `proc`,
  `sys`). It skips `apex`, `binderfs`, `vendor`, `system`,
  `system_ext`, `linkerconfig` since libhybris doesn't run there
  anyway.

## Magisk policy persistence
Once the policies row is in `/data/adb/magisk.db`, it survives
emulator reboots and snapshot save/load. If you wipe the AVD
(`emulator -avd tawc -wipe-data`), repeat steps 4–6.
