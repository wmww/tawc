//! Server bindings for the `android_wlegl` Wayland protocol used by
//! libhybris's Wayland EGL platform to ship gralloc buffer handles.

pub mod android_wlegl {
    pub mod server {
        use wayland_server;
        use wayland_server::protocol::*;
        pub mod __interfaces {
            use wayland_server::backend as wayland_backend;
            use wayland_server::protocol::__interfaces::*;
            wayland_scanner::generate_interfaces!("protocols/android_wlegl.xml");
        }
        use self::__interfaces::*;

        wayland_scanner::generate_server_code!("protocols/android_wlegl.xml");
    }
}
