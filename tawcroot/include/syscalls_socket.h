/* AF_UNIX socket address translation for bind/connect/accept. See
 * syscalls_socket.c.
 */

#pragma once

/* Register the socket handler set in the dispatch table. Called from
 * tawcroot_dispatch_init. */
void tawcroot_socket_register(void);
