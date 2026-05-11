//! Server side of the `tawc_gfxstream` Wayland protocol.
//!
//! The chroot presents a gfxstream ColorBuffer id. Because the kumquat
//! server and compositor live in the same process, we can resolve that
//! id to an `AHardwareBuffer*` and reuse the android_wlegl import path.

#[cfg(target_os = "android")]
unsafe extern "C" {
    /// Return the ColorBuffer's AHB with refcount += 1, or null on miss.
    fn tawc_gfxstream_lookup_ahb(handle: u32) -> *mut ndk_sys::AHardwareBuffer;
}

#[cfg(not(target_os = "android"))]
unsafe fn tawc_gfxstream_lookup_ahb(_handle: u32) -> *mut ndk_sys::AHardwareBuffer {
    // Host test builds do not link libgfxstream_backend.so.
    std::ptr::null_mut()
}

use log::{error, info};
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};

use crate::compositor::TawcState;
use crate::protocol::tawc_gfxstream::server::{
    tawc_gfxstream::{self, TawcGfxstream},
};
use crate::wlegl::WleglBufferData;

// ---------------------------------------------------------------------------
// Global dispatch
// ---------------------------------------------------------------------------

impl GlobalDispatch<TawcGfxstream, ()> for TawcState {
    fn bind(
        _state: &mut Self,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: New<TawcGfxstream>,
        _global_data: &(),
        data_init: &mut DataInit<'_, Self>,
    ) {
        data_init.init(resource, ());
        info!("gfxstream_present: client bound tawc_gfxstream");
    }
}

impl Dispatch<TawcGfxstream, ()> for TawcState {
    fn request(
        _state: &mut Self,
        _client: &Client,
        resource: &TawcGfxstream,
        request: tawc_gfxstream::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            tawc_gfxstream::Request::CreateBuffer {
                id,
                colorbuffer_id,
                width,
                height,
                format,
            } => {
                // SAFETY: tawc_gfxstream_lookup_ahb is a plain C entry
                // point inside libgfxstream_backend.so. Returns a
                // refcounted AHB on success or null on miss.
                let ahb = unsafe { tawc_gfxstream_lookup_ahb(colorbuffer_id) };
                if ahb.is_null() {
                    error!(
                        "gfxstream_present: unknown colorbuffer_id={}; killing client",
                        colorbuffer_id,
                    );
                    resource.post_error(
                        tawc_gfxstream::Error::UnknownColorbuffer,
                        format!("colorbuffer_id={} not in FrameBuffer", colorbuffer_id),
                    );
                    return;
                }
                // Adopt the AHB ref; WleglBufferData releases it when
                // the wl_buffer is destroyed.
                let data = WleglBufferData::from_ahb(ahb, width, height);
                let buffer = data_init.init(id, data);
                info!(
                    "gfxstream_present: bound colorbuffer_id={} as wl_buffer id={} {}x{} fmt=0x{:08x}",
                    colorbuffer_id,
                    buffer.id().protocol_id(),
                    width,
                    height,
                    format,
                );
            }
            tawc_gfxstream::Request::Destroy => {
                // Destructor; live wl_buffers stay alive until they're
                // destroyed independently. Smithay tears down the
                // resource for us.
            }
        }
    }
}

// `crate::wlegl` provides Dispatch<WlBuffer, WleglBufferData>; both
// AHB-backed protocols share that userdata type.
