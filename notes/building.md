# Building and Deploying

## Environment

```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk  # JDK 26 crashes Kotlin Gradle plugin 2.1.20
export ANDROID_NDK_HOME=/home/ai/android-sdk/ndk/27.2.12479018
export ANDROID_HOME=/home/ai/android-sdk
```

## Build the APK

```bash
cd server && JAVA_HOME=/usr/lib/jvm/java-21-openjdk ./gradlew assembleDebug
```

The Gradle build invokes `cargo ndk` for the Rust compositor automatically.

## Install and Launch

```bash
adb install -r server/app/build/outputs/apk/debug/app-debug.apk && \
adb shell am force-stop me.phie.tawc && \
adb shell am start -n me.phie.tawc/.MainActivity
```

After reinstalling: the compositor restarts with a new Wayland socket. Any running
chroot clients (Firefox, etc.) will be connected to the old socket and show black
screens. Kill them and relaunch.

## Full Build and Deploy

```bash
cd /home/ai/tawc/server/compositor && \
cargo ndk --target arm64-v8a --platform 29 -- build --release && \
cd ../.. && \
server/gradlew -p server assembleDebug && \
adb install -r server/app/build/outputs/apk/debug/app-debug.apk && \
adb shell am force-stop me.phie.tawc && \
adb shell am start -n me.phie.tawc/.MainActivity
```

## Device Setup

SELinux must be permissive: `adb shell su -c setenforce 0` (resets on reboot).

## Build System Details

- Rust compositor cross-compiled for `aarch64-linux-android` via `cargo-ndk`
- Gradle invokes cargo-ndk, copies `.so` into `jniLibs/arm64-v8a/`
- Target API level: 29 (Android 10) minimum
- libxkbcommon cross-compiled from source with NDK toolchain
- wayland-rs uses pure Rust backend (no libwayland dependency)

Client-side WSI layer:
- Cross-compiled for `aarch64-linux-gnu` (glibc, for chroot)
- Links against libhybris
- Installed in chroot's library path

## libhybris

Our fork lives at `./libhybris` (clone with `git clone https://github.com/wmww/libhybris.git ./libhybris`).

Build and install to the phone's chroot:
```bash
bash client/build-libhybris          # incremental build
bash client/build-libhybris --clean   # full reconfigure
```

This tars the local `./libhybris` source, pushes it to the phone, and builds inside the chroot. Edit `./libhybris` locally, then re-run the script to deploy.

## Debug App & Integration Tests

See [testing.md](testing.md) for full details.

```bash
# Build debug app on phone:
bash testing/build-debug-app.sh

# Run integration tests (from host):
cd testing/integration && cargo test -- --nocapture --test-threads=1
```
