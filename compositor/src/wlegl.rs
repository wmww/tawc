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
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use smithay::backend::renderer::gles::{GlesRenderer, GlesTexture};
use smithay::backend::renderer::{ExternalBuffer, ExternalBufferData, ExternalBufferImportError};
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;
use smithay::reexports::wayland_server::{
    Client, DataInit, Dispatch, DisplayHandle, GlobalDispatch, New, Resource,
};
use smithay::utils::{Buffer as BufferCoord, Rectangle, Size};
use smithay::wayland::compositor::SurfaceData;

use crate::compositor::TawcState;
use crate::gl_import::AhbTextureImporter;
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

/// Which Wayland protocol minted the wl_buffer holding the AHB. The renderer
/// reads this to pick a debug tint colour.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BufferOrigin {
    /// `android_wlegl` — libhybris's Wayland EGL platform (chroot
    /// client renders with vendor GLES into a gralloc1 AHB).
    Hybris,
    /// `tawc_gfxstream` — gfxstream-bridge custom Vulkan WSI (chroot
    /// client renders with mesa+gfxstream into a kumquat-allocated AHB).
    #[cfg(feature = "gfxstream")]
    Gfxstream,
}

static WLEGL_CREATE_BUFFER_TOTAL: AtomicU64 = AtomicU64::new(0);
static WLEGL_IMPORT_TEXTURE_TOTAL: AtomicU64 = AtomicU64::new(0);
static LAST_WLEGL_WIDTH: AtomicU32 = AtomicU32::new(0);
static LAST_WLEGL_HEIGHT: AtomicU32 = AtomicU32::new(0);
static LAST_WLEGL_FORMAT: AtomicU32 = AtomicU32::new(0);

pub struct WleglDebugCounters {
    pub create_buffer_total: u64,
    pub import_texture_total: u64,
    pub last_width: u32,
    pub last_height: u32,
    pub last_format: u32,
}

pub fn debug_counters() -> WleglDebugCounters {
    WleglDebugCounters {
        create_buffer_total: WLEGL_CREATE_BUFFER_TOTAL.load(Ordering::Relaxed),
        import_texture_total: WLEGL_IMPORT_TEXTURE_TOTAL.load(Ordering::Relaxed),
        last_width: LAST_WLEGL_WIDTH.load(Ordering::Relaxed),
        last_height: LAST_WLEGL_HEIGHT.load(Ordering::Relaxed),
        last_format: LAST_WLEGL_FORMAT.load(Ordering::Relaxed),
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
    pub has_alpha: bool,
    pub origin: BufferOrigin,
    importer: AhbTextureImporter,
    /// Imported on first render; reused thereafter.
    pub texture: Mutex<Option<GlesTexture>>,
}

fn android_format_has_alpha(format: u32) -> bool {
    // Android/HAL common formats: RGBA_8888=1, RGBX_8888=2, RGB_888=3,
    // RGB_565=4. Treat unknown/implementation-defined formats as alpha-capable
    // unless we have an explicit no-alpha value.
    !matches!(format, 2 | 3 | 4)
}

impl WleglBufferData {
    pub fn ahb_raw(&self) -> *mut ndk_sys::AHardwareBuffer {
        self.ahb
    }

    /// Wrap an externally-acquired AHardwareBuffer. Caller transfers
    /// **one** ref to us — `Drop` releases it. Used by the
    /// gfxstream-bridge custom Vulkan WSI path
    /// (`crate::gfxstream_present`) to adopt an AHB allocated by the
    /// kumquat-side gfxstream renderer and resolved by colorbuffer
    /// id via `tawc_gfxstream_lookup_ahb`.
    ///
    /// Not used by the android_wlegl path, which has its own
    /// gralloc1-import construction via `tawc_wlegl_import` and tags
    /// the resulting buffer with [`BufferOrigin::Hybris`] inline.
    #[cfg(feature = "gfxstream")]
    pub fn from_ahb(
        ahb: *mut ndk_sys::AHardwareBuffer,
        width: i32,
        height: i32,
        importer: AhbTextureImporter,
    ) -> Self {
        Self {
            ahb,
            width,
            height,
            has_alpha: true,
            origin: BufferOrigin::Gfxstream,
            importer,
            texture: Mutex::new(None),
        }
    }
}

impl ExternalBuffer for WleglBufferData {
    fn dimensions(&self) -> Size<i32, BufferCoord> {
        Size::from((self.width, self.height))
    }

    fn has_alpha(&self) -> Option<bool> {
        Some(self.has_alpha)
    }

    fn y_inverted(&self) -> Option<bool> {
        Some(false)
    }

    fn as_any(&self) -> &dyn std::any::Any {
        self
    }

    fn import_gles(
        &self,
        renderer: &mut GlesRenderer,
        _surface: Option<&SurfaceData>,
        _damage: &[Rectangle<i32, BufferCoord>],
    ) -> Option<Result<GlesTexture, ExternalBufferImportError>> {
        if let Some(texture) = self.texture.lock().unwrap().clone() {
            return Some(Ok(texture));
        }

        let texture = (|| {
            unsafe {
                renderer
                    .egl_context()
                    .make_current()
                    .map_err(|err| -> ExternalBufferImportError { Box::new(err) })?;
            }
            let display = renderer.egl_context().display().get_display_handle();
            self.importer
                .import_ahb(renderer, **display, self.ahb_raw(), self.width, self.height)
                .map_err(|err| -> ExternalBufferImportError {
                    std::io::Error::new(std::io::ErrorKind::Other, err).into()
                })
        })();
        let texture = match texture {
            Ok(texture) => texture,
            Err(err) => return Some(Err(err)),
        };
        *self.texture.lock().unwrap() = Some(texture.clone());
        WLEGL_IMPORT_TEXTURE_TOTAL.fetch_add(1, Ordering::Relaxed);
        Some(Ok(texture))
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
// immutable after construction. The Mutex<Option<GlesTexture>> field is also
// safe to send, but GlesTexture drop must happen on the GL thread — this is
// guaranteed because WleglBufferData lives as wl_buffer user-data, and
// resource cleanup runs during dispatch_clients on the compositor thread
// (which is the GL thread).
unsafe impl Send for WleglBufferData {}
unsafe impl Sync for WleglBufferData {}

/// Look up the WleglBufferData user-data attached to a wl_buffer, if any.
pub fn wlegl_buffer_data(buffer: &WlBuffer) -> Option<&WleglBufferData> {
    buffer
        .data::<ExternalBufferData>()
        .and_then(|data| data.get::<WleglBufferData>())
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
    }
}

impl Dispatch<AndroidWlegl, ()> for TawcState {
    fn request(
        state: &mut Self,
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
                        format!(
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
                WLEGL_CREATE_BUFFER_TOTAL.fetch_add(1, Ordering::Relaxed);
                LAST_WLEGL_WIDTH.store(w_u, Ordering::Relaxed);
                LAST_WLEGL_HEIGHT.store(h_u, Ordering::Relaxed);
                LAST_WLEGL_FORMAT.store(fmt_u, Ordering::Relaxed);

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
                    // post_error is fatal (kills the client), so skipping data_init.init() is safe.
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
                    has_alpha: android_format_has_alpha(fmt_u),
                    origin: BufferOrigin::Hybris,
                    importer: state.render.importer,
                    texture: Mutex::new(None),
                };
                data_init.init(id, ExternalBufferData::new(data));
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
impl Dispatch<WlBuffer, ExternalBufferData> for TawcState {
    fn request(
        _state: &mut Self,
        _client: &Client,
        _resource: &WlBuffer,
        _request: smithay::reexports::wayland_server::protocol::wl_buffer::Request,
        _data: &ExternalBufferData,
        _dhandle: &DisplayHandle,
        _data_init: &mut DataInit<'_, Self>,
    ) {
    }
}
