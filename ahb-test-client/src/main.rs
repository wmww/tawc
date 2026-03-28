//! Standalone AHB test client.
//! Allocates an AHardwareBuffer, fills with a checkerboard, and sends it
//! over a Unix socket to the compositor.
//!
//! Usage: ahb-test-client <socket-path>

use std::ffi::c_void;
use std::os::unix::io::AsRawFd;
use std::os::unix::net::UnixStream;
use std::ptr;

fn main() {
    let socket_path = std::env::args().nth(1).unwrap_or_else(|| {
        eprintln!("Usage: ahb-test-client <socket-path>");
        std::process::exit(1);
    });

    println!("[client] Connecting to {}...", socket_path);
    let stream = UnixStream::connect(&socket_path).unwrap_or_else(|e| {
        eprintln!("[client] Failed to connect: {}", e);
        std::process::exit(1);
    });
    println!("[client] Connected");

    // Allocate AHB
    let width: u32 = 256;
    let height: u32 = 256;

    let desc = ndk_sys::AHardwareBuffer_Desc {
        width,
        height,
        layers: 1,
        format: ndk_sys::AHardwareBuffer_Format::AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM.0,
        usage: ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE.0
            | ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN.0,
        stride: 0,
        rfu0: 0,
        rfu1: 0,
    };

    let mut buffer: *mut ndk_sys::AHardwareBuffer = ptr::null_mut();
    let ret = unsafe { ndk_sys::AHardwareBuffer_allocate(&desc, &mut buffer) };
    if ret != 0 {
        eprintln!("[client] AHardwareBuffer_allocate failed: {}", ret);
        std::process::exit(1);
    }
    println!("[client] AHB allocated: {}x{}", width, height);

    // Fill with green/yellow checkerboard (different from in-process test)
    let mut data_ptr: *mut c_void = ptr::null_mut();
    let ret = unsafe {
        ndk_sys::AHardwareBuffer_lock(
            buffer,
            ndk_sys::AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN.0,
            -1,
            ptr::null(),
            &mut data_ptr,
        )
    };
    if ret != 0 {
        eprintln!("[client] AHardwareBuffer_lock failed: {}", ret);
        std::process::exit(1);
    }

    let mut actual_desc = ndk_sys::AHardwareBuffer_Desc {
        width: 0, height: 0, layers: 0, format: 0, usage: 0, stride: 0, rfu0: 0, rfu1: 0,
    };
    unsafe { ndk_sys::AHardwareBuffer_describe(buffer, &mut actual_desc) };
    let stride = actual_desc.stride;

    let pixels = data_ptr as *mut u32;
    for y in 0..height {
        for x in 0..width {
            let checker = ((x / 32) + (y / 32)) % 2 == 0;
            // Green/yellow checkerboard to distinguish from in-process red/blue test
            let color: u32 = if checker {
                0xFF00FF00 // Green (R=0, G=0xFF, B=0, A=0xFF)
            } else {
                0xFF00FFFF // Yellow (R=0xFF, G=0xFF, B=0, A=0xFF)
            };
            unsafe {
                *pixels.add((y * stride + x) as usize) = color;
            }
        }
    }

    unsafe { ndk_sys::AHardwareBuffer_unlock(buffer, ptr::null_mut()) };
    println!("[client] AHB filled with green/yellow checkerboard");

    // Send AHB
    let ret = unsafe {
        ndk_sys::AHardwareBuffer_sendHandleToUnixSocket(buffer, stream.as_raw_fd())
    };
    if ret != 0 {
        eprintln!("[client] AHardwareBuffer_sendHandleToUnixSocket failed: {}", ret);
        std::process::exit(1);
    }
    println!("[client] AHB sent! Keeping alive for 30 seconds...");

    // Keep buffer alive
    std::thread::sleep(std::time::Duration::from_secs(30));

    unsafe { ndk_sys::AHardwareBuffer_release(buffer) };
    println!("[client] Done");
}
