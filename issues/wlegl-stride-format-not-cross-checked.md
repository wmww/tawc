# `android_wlegl::create_buffer` accepts client-supplied stride/format/usage without cross-checking the handle

## Summary

The protocol-supplied `width / height / stride / format / usage` from
`android_wlegl.create_buffer` are passed verbatim into
`AHardwareBuffer_Desc` in `tawc_wlegl_import` and handed to
`AHardwareBuffer_createFromHandle(REGISTER)`. The compositor doesn't
validate these against the actual handle.

In normal operation libhybris's wayland-egl plugin sends matching
values (gralloc allocates the buffer; libhybris reads stride/format
back from the gralloc buffer info before posting). But a malicious or
buggy client could lie — e.g. announce `format=RGBA_8888` while the
handle is a YUV buffer — and we'd register the AHB with mismatched
metadata. Subsequent `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)`
returns a texture sampled with the wrong format.

The corruption stays inside the AHB created from this handle (no
cross-buffer leak); the worst direct consequence is wrong colors /
crashes inside the GPU driver when the texture is sampled.

## Impact

Low for benign clients. Trust boundary issue if we ever expose the
Wayland socket to less-trusted code in the chroot.

## Fix

After `AHardwareBuffer_createFromHandle` returns, call
`AHardwareBuffer_describe` and verify the returned `Desc` matches the
client-supplied values. Reject (`AHardwareBuffer_release` + protocol
error) on mismatch.

## Where

`server/compositor/native/wlegl_import.c` in `tawc_wlegl_import`,
right after the success branch of `g_create_from_handle`.
