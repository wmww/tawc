fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    if target_os == "android" {
        // Locally-built static xkbcommon for the compositor.
        // arm64 build lives in builddir/, x86_64 (emulator) in builddir-x86_64/.
        let target_arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
        let xkb_dir = match target_arch.as_str() {
            "aarch64" => "/home/ai/libxkbcommon/builddir",
            "x86_64" => "/home/ai/libxkbcommon/builddir-x86_64",
            other => panic!("unsupported android target arch: {}", other),
        };
        println!("cargo:rustc-link-search=native={}", xkb_dir);
        println!("cargo:rustc-link-lib=static=xkbcommon");

        // C helper that turns an android_wlegl native_handle_t into an
        // AHardwareBuffer via AHardwareBuffer_createFromHandle (dlsym'd at
        // runtime from libnativewindow.so). Uses only the public NDK
        // surface — no vendored platform headers needed.
        cc::Build::new()
            .file("native/wlegl_import.c")
            .flag("-std=c11")
            .flag("-Wno-unused-parameter")
            .compile("tawc_wlegl_import");

        // Dependencies of the C helper.
        // libhardware isn't in the NDK — we dlopen it at runtime in the helper.
        println!("cargo:rustc-link-lib=dylib=log");
        println!("cargo:rustc-link-lib=dylib=dl");

        println!("cargo:rerun-if-changed=native/wlegl_import.c");
    }
}
