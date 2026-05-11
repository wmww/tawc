//! AHB to dmabuf-fd export hook for kumquat blob responses.
//!
//! Host-visible gfxstream allocations arrive as AHBs. The kumquat
//! protocol must return one fd with each `RESOURCE_CREATE_BLOB`, and
//! the chroot mmaps that fd for CPU-visible Vulkan memory. We dlsym
//! `AHardwareBuffer_getNativeHandle` from `libnativewindow.so`, dup
//! `native_handle_t.data[0]`, and hand the dup to rutabaga.

use std::ffi::CStr;

use log::{error, info, warn};

// ---------------------------------------------------------------------------
// libnativewindow.so FFI shim
// ---------------------------------------------------------------------------

/// AOSP's `system/core/libcutils/include/cutils/native_handle.h` shape.
/// Trailing `data` is a flex array: `numFds` raw fds followed by
/// `numInts` raw ints.
#[repr(C)]
struct NativeHandle {
    version: std::os::raw::c_int,
    num_fds: std::os::raw::c_int,
    num_ints: std::os::raw::c_int,
    data: [std::os::raw::c_int; 0],
}

type FnAhbGetNativeHandle =
    unsafe extern "C" fn(*const ndk_sys::AHardwareBuffer) -> *const NativeHandle;

struct NativeWindowSyms {
    get_native_handle: FnAhbGetNativeHandle,
}

static NATIVE_WINDOW: std::sync::OnceLock<Option<NativeWindowSyms>> =
    std::sync::OnceLock::new();

fn native_window() -> Option<&'static NativeWindowSyms> {
    NATIVE_WINDOW
        .get_or_init(|| unsafe { load_native_window() })
        .as_ref()
}

unsafe fn load_native_window() -> Option<NativeWindowSyms> {
    let libname = c"libnativewindow.so";
    let lib = libc::dlopen(libname.as_ptr(), libc::RTLD_NOW | libc::RTLD_GLOBAL);
    if lib.is_null() {
        let err = CStr::from_ptr(libc::dlerror()).to_string_lossy().into_owned();
        error!("ahb_export: dlopen({:?}) failed: {}", libname, err);
        return None;
    }
    let sym = libc::dlsym(lib, c"AHardwareBuffer_getNativeHandle".as_ptr());
    if sym.is_null() {
        let err = CStr::from_ptr(libc::dlerror()).to_string_lossy().into_owned();
        error!("ahb_export: dlsym(AHardwareBuffer_getNativeHandle) failed: {}", err);
        return None;
    }
    Some(NativeWindowSyms {
        get_native_handle: std::mem::transmute::<*mut std::os::raw::c_void, FnAhbGetNativeHandle>(
            sym,
        ),
    })
}

// ---------------------------------------------------------------------------
// rutabaga AhbExportFn callback
// ---------------------------------------------------------------------------

/// Rutabaga `AhbExportFn` callback. Runs on the kumquat server thread
/// inside the patched `Gfxstream::export_blob`'s PLATFORM_AHB branch
/// (see `deps/rutabaga-patches/rutabaga_gfx/01-drop-nativewindow-dep.patch`).
///
/// Steps:
///   1. dlsym `AHardwareBuffer_getNativeHandle`, walk the
///      `native_handle_t` to get its first dmabuf fd (`data[0]`).
///   2. `dup` it so closing the kumquat-side `MesaHandle` doesn't
///      affect downstream consumers.
///   3. Return the dup'd fd. The kumquat layer wraps it as
///      `MEM_DMABUF` and ships it via SCM_RIGHTS to the chroot.
fn ahb_export_callback(ahb_os_handle: i64) -> Result<i32, ()> {
    let ahb = ahb_os_handle as *mut ndk_sys::AHardwareBuffer;
    if ahb.is_null() {
        error!("ahb_export: AHB export with null pointer");
        return Err(());
    }

    let syms = native_window().ok_or(())?;

    // SAFETY: ahb is a live AHardwareBuffer (gfxstream just handed it
    // back via stream_renderer_export_blob). `getNativeHandle` returns
    // a `const native_handle_t*` whose lifetime is bounded by the AHB's
    // refcount; we dup the fd before returning so the dup outlives any
    // caller-side ref dropping.
    let nh = unsafe { (syms.get_native_handle)(ahb) };
    if nh.is_null() {
        error!("ahb_export: AHardwareBuffer_getNativeHandle returned null");
        return Err(());
    }
    let (num_fds, data_ptr) = unsafe { ((*nh).num_fds, (*nh).data.as_ptr()) };
    if num_fds < 1 {
        error!("ahb_export: native_handle_t with numFds={} (need >= 1)", num_fds);
        return Err(());
    }
    // SAFETY: data_ptr is a valid flex array; `data[0]` is the backing
    // gralloc dmabuf (other fds are gralloc bookkeeping, not memory).
    let backing_fd = unsafe { *data_ptr };
    if backing_fd < 0 {
        error!("ahb_export: native_handle_t.data[0] = {} (not a valid fd)", backing_fd);
        return Err(());
    }
    // SAFETY: backing_fd is currently owned by the AHB. dup gives us
    // an independent fd referring to the same description / inode.
    let ship_fd = unsafe { libc::dup(backing_fd) };
    if ship_fd < 0 {
        let e = std::io::Error::last_os_error();
        error!("ahb_export: dup(data[0]={}) failed: {}", backing_fd, e);
        return Err(());
    }
    Ok(ship_fd)
}

/// Install the AHB export hook so gfxstream's PLATFORM_AHB path routes
/// back through us instead of the AOSP-only `nativewindow` Rust crate.
/// Idempotent; second call logs and returns.
///
/// Called from `nativeStartCompositor` before the kumquat thread starts
/// accepting connections — first AHB allocation can land immediately,
/// no race window.
pub fn install_hook() {
    #[cfg(target_os = "android")]
    {
        match kumquat_virtio::rutabaga_gfx::set_ahb_export_callback(ahb_export_callback) {
            Ok(()) => info!("ahb_export: hook installed"),
            Err(()) => warn!("ahb_export: hook already installed (idempotent)"),
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        info!("ahb_export: hook skipped (not an android build)");
    }
}
