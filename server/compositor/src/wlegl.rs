//! android_wlegl protocol handler: reconstruct client-allocated gralloc buffers
//! (sent as native_handle_t fds + ints) into ANativeWindowBuffers via vendor
//! gralloc1, and expose them as wl_buffers.
//!
//! Version 2 of the protocol is advertised, but get_server_buffer_handle is
//! rejected — we use client-side allocation only
//! (libhybris --disable-wayland_serverside_buffers).
//!
//! Handles travel round-trip entirely in gralloc1 land: the client's libhybris
//! allocates via vendor gralloc1, we import via the same vendor gralloc1, and
//! the compositor-side EGL driver consumes the resulting ANativeWindowBuffer.
//! The AHardwareBuffer / gralloc4 mapper path is explicitly avoided because
//! its private_handle_t format differs on modern devices.

use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::ptr;
use std::sync::Mutex;

use log::info;

use smithay::backend::renderer::gles::GlesTexture;
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};

use crate::compositor::TawcState;
use crate::protocol::android_wlegl::server::{
    android_wlegl::{self, AndroidWlegl},
    android_wlegl_handle::{self, AndroidWleglHandle},
};

// ---------------------------------------------------------------------------
// FFI: C helper in native/wlegl_import.c
// ---------------------------------------------------------------------------

unsafe extern "C" {
    /// Build an AHardwareBuffer from the wire handle via
    /// AHardwareBuffer_createFromHandle. Returns NULL on failure.
    /// On success, ownership is transferred to the caller; release with
    /// `tawc_wlegl_buffer_release`.
    fn tawc_wlegl_import(
        width: u32,
        height: u32,
        stride: u32,
        format: u32,
        usage: u64,
        fds: *const RawFd,
        num_fds: i32,
        ints: *const i32,
        num_ints: i32,
    ) -> *mut ndk_sys::AHardwareBuffer;

    fn tawc_wlegl_buffer_release(ahb: *mut ndk_sys::AHardwareBuffer);
}

// ---------------------------------------------------------------------------
// Protocol user-data
// ---------------------------------------------------------------------------

/// Accumulator for fds + ints for a single native_handle_t in flight.
/// Filled by `add_fd` requests; consumed by `create_buffer`.
pub struct WleglHandleData {
    pub inner: Mutex<WleglHandleInner>,
}

pub struct WleglHandleInner {
    pub expected_fds: i32,
    pub fds: Vec<OwnedFd>,
    pub ints: Vec<i32>,
}

impl WleglHandleData {
    fn new(expected_fds: i32, ints: Vec<i32>) -> Self {
        Self {
            inner: Mutex::new(WleglHandleInner {
                expected_fds,
                fds: Vec::with_capacity(expected_fds.max(0) as usize),
                ints,
            }),
        }
    }
}

/// Per-wl_buffer state: an AHardwareBuffer + lazily-imported GlesTexture.
///
/// The AHB is kept alive for as long as the wl_buffer exists; Drop triggers
/// `AHardwareBuffer_release`, which closes its fds and frees the underlying
/// gralloc buffer.
pub struct WleglBufferData {
    ahb: *mut ndk_sys::AHardwareBuffer,
    pub width: i32,
    pub height: i32,
    /// Imported on first render; reused thereafter.
    pub texture: Mutex<Option<GlesTexture>>,
}

impl WleglBufferData {
    pub fn ahb_raw(&self) -> *mut ndk_sys::AHardwareBuffer {
        self.ahb
    }
}

impl Drop for WleglBufferData {
    fn drop(&mut self) {
        if !self.ahb.is_null() {
            unsafe { tawc_wlegl_buffer_release(self.ahb) };
            self.ahb = ptr::null_mut();
        }
    }
}

// SAFETY: AHardwareBuffer refcounting is thread-safe; the pointer itself is
// immutable after construction.
unsafe impl Send for WleglBufferData {}
unsafe impl Sync for WleglBufferData {}

/// Look up the WleglBufferData user-data attached to a wl_buffer, if any.
pub fn wlegl_buffer_data(buffer: &WlBuffer) -> Option<&WleglBufferData> {
    buffer.data::<WleglBufferData>()
}

// ---------------------------------------------------------------------------
// android_wlegl global dispatch
// ---------------------------------------------------------------------------

impl GlobalDispatch<AndroidWlegl, ()> for TawcState {
    fn bind(
        _state: &mut Self,
        _handle: &DisplayHandle,
        _client: &Client,
        resource: New<AndroidWlegl>,
        _global_data: &(),
        data_init: &mut DataInit<'_, Self>,
    ) {
        data_init.init(resource, ());
        info!("Client bound android_wlegl");
    }
}

impl Dispatch<AndroidWlegl, ()> for TawcState {
    fn request(
        _state: &mut Self,
        _client: &Client,
        resource: &AndroidWlegl,
        request: android_wlegl::Request,
        _data: &(),
        _dhandle: &DisplayHandle,
        data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            android_wlegl::Request::CreateHandle { id, num_fds, ints } => {
                if ints.len() % 4 != 0 {
                    resource.post_error(
                        android_wlegl::Error::BadValue,
                        "ints array length is not a multiple of 4",
                    );
                    return;
                }
                let ints_i32: Vec<i32> = ints
                    .chunks_exact(4)
                    .map(|c| i32::from_ne_bytes([c[0], c[1], c[2], c[3]]))
                    .collect();

                data_init.init(id, WleglHandleData::new(num_fds, ints_i32));
            }
            android_wlegl::Request::CreateBuffer {
                id,
                width,
                height,
                stride,
                format,
                usage,
                native_handle,
            } => {
                let handle_data = match native_handle.data::<WleglHandleData>() {
                    Some(d) => d,
                    None => {
                        resource.post_error(
                            android_wlegl::Error::BadHandle,
                            "native_handle has wrong user-data type",
                        );
                        return;
                    }
                };
                let mut inner = handle_data.inner.lock().unwrap();
                let num_fds_got = inner.fds.len() as i32;
                if num_fds_got != inner.expected_fds {
                    resource.post_error(
                        android_wlegl::Error::BadHandle,
                        &format!(
                            "expected {} fds, got {}",
                            inner.expected_fds, num_fds_got
                        ),
                    );
                    return;
                }

                let raw_fds: Vec<RawFd> =
                    inner.fds.iter().map(|f| f.as_raw_fd()).collect();
                let ints = inner.ints.clone();

                // Wire types are i32 (signed); cast through u32 first so high
                // bits of usage aren't sign-extended into the u64.
                let w_u = width as u32;
                let h_u = height as u32;
                let stride_u = stride as u32;
                let fmt_u = format as u32;
                let usage_u64 = (usage as u32) as u64;

                let ahb = unsafe {
                    tawc_wlegl_import(
                        w_u,
                        h_u,
                        stride_u,
                        fmt_u,
                        usage_u64,
                        raw_fds.as_ptr(),
                        raw_fds.len() as i32,
                        ints.as_ptr(),
                        ints.len() as i32,
                    )
                };
                if ahb.is_null() {
                    // Leave fds owned by WleglHandleData; they'll be closed
                    // on handle destroy.
                    resource.post_error(
                        android_wlegl::Error::BadHandle,
                        "AHardwareBuffer_createFromHandle failed",
                    );
                    return;
                }

                // REGISTER took ownership of the fds — forget our OwnedFds
                // so their Drop impl doesn't double-close.
                for fd in inner.fds.drain(..) {
                    std::mem::forget(fd);
                }
                drop(inner);

                let data = WleglBufferData {
                    ahb,
                    width,
                    height,
                    texture: Mutex::new(None),
                };
                data_init.init(id, data);
                info!(
                    "wlegl: create_buffer {}x{} stride={} fmt={} usage=0x{:x}",
                    width, height, stride, format, usage_u64
                );
            }
            android_wlegl::Request::GetServerBufferHandle { .. } => {
                resource.post_error(
                    android_wlegl::Error::BadValue,
                    "server-side buffer allocation is not supported",
                );
            }
        }
    }
}

impl Dispatch<AndroidWleglHandle, WleglHandleData> for TawcState {
    fn request(
        _state: &mut Self,
        _client: &Client,
        resource: &AndroidWleglHandle,
        request: android_wlegl_handle::Request,
        data: &WleglHandleData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Self>,
    ) {
        match request {
            android_wlegl_handle::Request::AddFd { fd } => {
                let mut inner = data.inner.lock().unwrap();
                if inner.fds.len() as i32 >= inner.expected_fds {
                    resource.post_error(
                        android_wlegl_handle::Error::TooManyFds,
                        "too many fds",
                    );
                    return;
                }
                inner.fds.push(fd);
            }
            android_wlegl_handle::Request::Destroy => {
                // Any leftover fds are closed by OwnedFd's Drop.
            }
        }
    }
}

// wl_buffer dispatch — protocol requires it since we `new_id` one here.
impl Dispatch<WlBuffer, WleglBufferData> for TawcState {
    fn request(
        _state: &mut Self,
        _client: &Client,
        _resource: &WlBuffer,
        _request: smithay::reexports::wayland_server::protocol::wl_buffer::Request,
        _data: &WleglBufferData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Self>,
    ) {
    }
}
