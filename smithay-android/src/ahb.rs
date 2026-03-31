//! AHardwareBuffer allocation, CPU fill, and cross-process sharing via Unix sockets.

use std::ffi::c_void;
use std::os::unix::io::RawFd;
use std::ptr;

use log::info;

/// Wrapper around a raw AHardwareBuffer pointer with RAII release.
pub struct AhbBuffer {
    buffer: *mut ndk_sys::AHardwareBuffer,
    width: u32,
    height: u32,
}

unsafe impl Send for AhbBuffer {}

impl AhbBuffer {
    /// Allocate an RGBA8888 AHardwareBuffer with GPU sampled + GPU render + CPU write usage.
    pub fn allocate(width: u32, height: u32) -> Result<Self, String> {
        let desc = ndk_sys::AHardwareBuffer_Desc {
            width,
            height,
            layers: 1,
            format: ndk_sys::AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM.0,
            usage: ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE.0
                | ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT.0
                | ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN.0,
            stride: 0,
            rfu0: 0,
            rfu1: 0,
        };

        let mut buffer: *mut ndk_sys::AHardwareBuffer = ptr::null_mut();
        let ret = unsafe { ndk_sys::AHardwareBuffer_allocate(&desc, &mut buffer) };
        if ret != 0 {
            return Err(format!("AHardwareBuffer_allocate failed: {}", ret));
        }
        info!("AHB allocated: {}x{}, ptr={:?}", width, height, buffer);
        Ok(Self { buffer, width, height })
    }

    /// Fill the buffer with a red/blue checkerboard pattern via CPU lock.
    pub fn fill_test_pattern(&self) -> Result<(), String> {
        let mut data_ptr: *mut c_void = ptr::null_mut();
        let ret = unsafe {
            ndk_sys::AHardwareBuffer_lock(
                self.buffer,
                ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN.0,
                -1, // no fence
                ptr::null(), // entire buffer
                &mut data_ptr,
            )
        };
        if ret != 0 {
            return Err(format!("AHardwareBuffer_lock failed: {}", ret));
        }

        // Get actual stride from describe
        let mut desc = ndk_sys::AHardwareBuffer_Desc {
            width: 0, height: 0, layers: 0, format: 0, usage: 0, stride: 0, rfu0: 0, rfu1: 0,
        };
        unsafe { ndk_sys::AHardwareBuffer_describe(self.buffer, &mut desc) };
        let stride = desc.stride; // in pixels

        // Fill with 32x32 pixel checkerboard: red and blue
        let pixels = data_ptr as *mut u32;
        for y in 0..self.height {
            for x in 0..self.width {
                let checker = ((x / 32) + (y / 32)) % 2 == 0;
                // RGBA8888: R, G, B, A in memory order
                let color: u32 = if checker {
                    0xFF0000FF // Red (R=0xFF, G=0, B=0, A=0xFF)
                } else {
                    0xFFFF0000 // Blue (R=0, G=0, B=0xFF, A=0xFF)
                };
                unsafe {
                    *pixels.add((y * stride + x) as usize) = color;
                }
            }
        }

        let ret = unsafe { ndk_sys::AHardwareBuffer_unlock(self.buffer, ptr::null_mut()) };
        if ret != 0 {
            return Err(format!("AHardwareBuffer_unlock failed: {}", ret));
        }
        info!("AHB filled with test pattern");
        Ok(())
    }

    /// Send this AHB over a Unix socket fd.
    pub fn send_to_socket(&self, fd: RawFd) -> Result<(), String> {
        let ret = unsafe {
            ndk_sys::AHardwareBuffer_sendHandleToUnixSocket(self.buffer, fd)
        };
        if ret != 0 {
            return Err(format!("AHardwareBuffer_sendHandleToUnixSocket failed: {}", ret));
        }
        info!("AHB sent over socket fd={}", fd);
        Ok(())
    }

    /// Receive an AHB from a Unix socket fd.
    pub fn recv_from_socket(fd: RawFd) -> Result<Self, String> {
        let mut buffer: *mut ndk_sys::AHardwareBuffer = ptr::null_mut();
        let ret = unsafe {
            ndk_sys::AHardwareBuffer_recvHandleFromUnixSocket(fd, &mut buffer)
        };
        if ret != 0 {
            return Err(format!("AHardwareBuffer_recvHandleFromUnixSocket failed: {}", ret));
        }

        // Get dimensions from the received buffer
        let mut desc = ndk_sys::AHardwareBuffer_Desc {
            width: 0, height: 0, layers: 0, format: 0, usage: 0, stride: 0, rfu0: 0, rfu1: 0,
        };
        unsafe { ndk_sys::AHardwareBuffer_describe(buffer, &mut desc) };
        info!("AHB received: {}x{}, format={}, ptr={:?}", desc.width, desc.height, desc.format, buffer);

        Ok(Self {
            buffer,
            width: desc.width,
            height: desc.height,
        })
    }

    pub fn as_raw(&self) -> *mut ndk_sys::AHardwareBuffer {
        self.buffer
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }
}

impl Drop for AhbBuffer {
    fn drop(&mut self) {
        if !self.buffer.is_null() {
            unsafe { ndk_sys::AHardwareBuffer_release(self.buffer) };
            info!("AHB released: {:?}", self.buffer);
        }
    }
}
