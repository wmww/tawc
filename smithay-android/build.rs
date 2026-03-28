fn main() {
    // Tell the linker where to find libxkbcommon for Android builds
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "android" {
        println!("cargo:rustc-link-search=native=/home/ai/libxkbcommon/builddir");
        println!("cargo:rustc-link-lib=static=xkbcommon");
    }
}
