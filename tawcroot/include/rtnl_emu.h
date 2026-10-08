/* NETLINK_ROUTE emulation for kernels that deny it to the app uid.
 * See rtnl_emu.c.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* One guest iovec: `base` is a GUEST pointer. */
struct tawc_rtnl_iov {
	uint64_t base;
	uint64_t len;
};

/* True iff the kernel-reported sockaddr `addr` (`len` bytes) names one
 * of our rtnetlink stubs. */
int tawcroot_rtnl_is_stub_name(const void *addr, long len);

/* True iff `fd` is one of our rtnetlink stub sockets. */
int tawcroot_rtnl_is_stub(int fd);

/* True iff this process's kernel denies NETLINK_ROUTE bind (Android
 * app domain), probed once. */
int tawcroot_rtnl_denied(void);

/* Forget the probe result. Test teardown only. */
void tawcroot_rtnl_reset(void);

/* Create a stub for socket(AF_NETLINK, `type`, NETLINK_ROUTE): fd or
 * -errno. SOCK_CLOEXEC / SOCK_NONBLOCK in `type` are kept. */
long tawcroot_rtnl_open_stub(long type);

/* Handle the datagram the guest sent on stub `fd`, gathered from
 * `iov`. Queues the replies on the stub and returns the byte count, or
 * -errno. */
long tawcroot_rtnl_send(int fd, const struct tawc_rtnl_iov *iov,
			size_t n_iov);

/* The port id getsockname reports for a stub (and replies carry). */
unsigned int tawcroot_rtnl_port_id(void);
