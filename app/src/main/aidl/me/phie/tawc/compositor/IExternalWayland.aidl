// Exported fd source for Wayland clients outside TAWC's uid.
//
// A Termux process cannot connect() to `share/wayland-0` (DAC traversal
// into app-private data + SELinux cross-app connect). Instead it asks for
// one end of a fresh socketpair; the other end is adopted by the
// compositor. Binder only ever carries the descriptor.
//
// See notes/android.md ("Wayland Socket Sharing") and
// plans/external-wayland-clients.md.
package me.phie.tawc.compositor;

import android.os.ParcelFileDescriptor;

interface IExternalWayland {
    /**
     * Returns the far end of a fresh SOCK_STREAM socketpair. The
     * compositor keeps the near end and inserts it into its Wayland
     * display as a client. Throws SecurityException for a disallowed
     * caller and IllegalStateException if the fd could not be accepted.
     */
    ParcelFileDescriptor openClient();
}
