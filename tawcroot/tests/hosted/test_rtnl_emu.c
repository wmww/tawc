/* Hosted tests for rtnl_emu.c, the NETLINK_ROUTE stub that stands in
 * where the kernel denies apps rtnetlink bind() and RTM_GETLINK.
 *
 * The host kernel denies neither, so a raw-syscall hook plays Android's
 * policy: bind() of a sockaddr_nl and RTM_GETLINK sends on real netlink
 * sockets fail EACCES. Guests drive the stub through the trapped
 * handlers (th_sys) and read replies with the untrapped native recvmsg;
 * results are checked against the host glibc's own interface view. */

#include <cleat/test.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hosted.h"
#include "raw_syscall_host.h"

#include "rtnl_emu.h"
#include "sysnr.h"

static int deny_getlink;

static int is_netlink_fd(long fd)
{
	int dom = 0;
	socklen_t len = sizeof dom;
	return getsockopt((int)fd, SOL_SOCKET, SO_DOMAIN, &dom, &len) == 0 &&
	       dom == AF_NETLINK;
}

/* Android's untrusted_app policy, as far as rtnl_emu can tell. */
static bool android_policy_hook(long nr, const long args[6], long *ret)
{
	if (nr == TAWC_SYS_bind && args[1] &&
	    ((const struct sockaddr *)args[1])->sa_family == AF_NETLINK &&
	    is_netlink_fd(args[0])) {
		*ret = -EACCES;
		return true;
	}
	if (nr == TAWC_SYS_sendto && deny_getlink && is_netlink_fd(args[0]) &&
	    (size_t)args[2] >= sizeof(struct nlmsghdr) &&
	    ((const struct nlmsghdr *)args[1])->nlmsg_type == RTM_GETLINK) {
		*ret = -EACCES;
		return true;
	}
	return false;
}

static void android_on(int getlink_denied)
{
	tawcroot_rtnl_reset();
	deny_getlink = getlink_denied;
	tawcroot_test_raw_hook = android_policy_hook;
}

static void android_off(void)
{
	tawcroot_test_raw_hook = NULL;
	tawcroot_rtnl_reset();
}

struct reply {
	int  n_links;
	int  n_other;
	int  done;
	int  error;      /* NLMSG_ERROR code, 1 if none seen */
	int  bad_pid;    /* a reply not addressed to our port id */
	int  bad_src;    /* a datagram not "from the kernel" */
	int  lo_has_mac; /* IFLA_ADDRESS on lo: only the real kernel's */
	char names[64][IF_NAMESIZE];
	int  index[64];
};

/* Read replies for `seq` until NLMSG_DONE / NLMSG_ERROR. They were all
 * queued before the send returned, so MSG_DONTWAIT never races. */
static void read_replies(int fd, uint32_t seq, struct reply *r)
{
	memset(r, 0, sizeof *r);
	r->error = 1;
	uint32_t port = tawcroot_rtnl_port_id();
	static char buf[16384];
	while (!r->done) {
		struct sockaddr_nl src;
		struct iovec iov = { buf, sizeof buf };
		struct msghdr mh = { .msg_name = &src,
				     .msg_namelen = sizeof src,
				     .msg_iov = &iov, .msg_iovlen = 1 };
		memset(&src, 0xff, sizeof src);
		ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT);
		if (n <= 0) return;
		if (mh.msg_namelen != sizeof src || src.nl_pid != 0 ||
		    src.nl_groups != 0)
			r->bad_src = 1;
		int left = (int)n;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
		     NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_seq != seq) continue;
			if (h->nlmsg_pid != port) r->bad_pid = 1;
			if (h->nlmsg_type == NLMSG_DONE) {
				r->done = 1;
			} else if (h->nlmsg_type == NLMSG_ERROR) {
				r->error = ((struct nlmsgerr *)
					    NLMSG_DATA(h))->error;
				r->done = 1;
			} else if (h->nlmsg_type == RTM_NEWLINK &&
				   r->n_links < 64) {
				struct ifinfomsg *ifi = NLMSG_DATA(h);
				int i = r->n_links++;
				int mac = 0;
				int al = (int)IFLA_PAYLOAD(h);
				for (struct rtattr *a = IFLA_RTA(ifi);
				     RTA_OK(a, al); a = RTA_NEXT(a, al)) {
					if (a->rta_type == IFLA_ADDRESS)
						mac = 1;
					if (a->rta_type == IFLA_IFNAME)
						strncpy(r->names[i], RTA_DATA(a),
							IF_NAMESIZE - 1);
				}
				r->index[i] = ifi->ifi_index;
				if (!strcmp(r->names[i], "lo") && mac)
					r->lo_has_mac = 1;
			} else {
				r->n_other++;
			}
		}
	}
}

static long nl_socket(TestCtx *test_ctx, int type)
{
	long fd = th_sys(TAWC_SYS_socket, AF_NETLINK, type, NETLINK_ROUTE,
			 0, 0, 0);
	test_true(fd >= 0);
	return fd;
}

static long send_getlink(TestCtx *test_ctx, long fd, uint16_t flags,
			 uint32_t seq, int32_t index)
{
	struct {
		struct nlmsghdr  h;
		struct ifinfomsg m;
	} rq;
	memset(&rq, 0, sizeof rq);
	rq.h.nlmsg_len = NLMSG_LENGTH(sizeof rq.m);
	rq.h.nlmsg_type = RTM_GETLINK;
	rq.h.nlmsg_flags = NLM_F_REQUEST | flags;
	rq.h.nlmsg_seq = seq;
	rq.m.ifi_index = index;
	/* glibc's shape: sendto with the kernel's sockaddr_nl. */
	struct sockaddr_nl k = { .nl_family = AF_NETLINK };
	return th_sys(TAWC_SYS_sendto, fd, &rq, rq.h.nlmsg_len, 0,
		      &k, sizeof k);
}

/* Where the kernel allows rtnetlink the guest keeps a real socket. */
test(hosted_rtnl_real_socket_when_allowed)
{
	th_view v;
	th_setup(&v, "rtnl");
	tawcroot_rtnl_reset();
	long fd = nl_socket(test_ctx, SOCK_RAW);
	test_false(tawcroot_rtnl_is_stub((int)fd));
	close((int)fd);
	th_teardown(&v);
}

/* glibc's __netlink_open: socket, bind, getsockname for the port id the
 * replies must carry. socket flags survive onto the stub. */
test(hosted_rtnl_stub_bind_and_name)
{
	th_view v;
	th_setup(&v, "rtnl");
	android_on(1);
	long fd = nl_socket(test_ctx, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK);
	test_true(tawcroot_rtnl_is_stub((int)fd));
	test_true(fcntl((int)fd, F_GETFL) & O_NONBLOCK);
	test_true(fcntl((int)fd, F_GETFD) & FD_CLOEXEC);

	struct sockaddr_nl snl = { .nl_family = AF_NETLINK,
				   .nl_groups = RTMGRP_IPV4_IFADDR };
	test_int_eq(th_sys(TAWC_SYS_bind, fd, &snl, sizeof snl, 0, 0, 0), 0);
	struct sockaddr_nl k = { .nl_family = AF_NETLINK };
	test_int_eq(th_sys(TAWC_SYS_connect, fd, &k, sizeof k, 0, 0, 0), 0);

	struct sockaddr_nl got;
	memset(&got, 0xff, sizeof got);
	unsigned int len = sizeof got;
	test_int_eq(th_sys(TAWC_SYS_getsockname, fd, &got, &len, 0, 0, 0), 0);
	test_int_eq(len, sizeof got);
	test_int_eq(got.nl_family, AF_NETLINK);
	test_int_eq(got.nl_pid, tawcroot_rtnl_port_id());

	close((int)fd);
	android_off();
	th_teardown(&v);
}

/* Every interface glibc sees with an address comes back from the
 * synthesized RTM_GETLINK dump, under the same index and name. */
test(hosted_rtnl_getlink_dump_synthesized)
{
	th_view v;
	th_setup(&v, "rtnl");
	android_on(1);
	long fd = nl_socket(test_ctx, SOCK_RAW);

	test_true(send_getlink(test_ctx, fd, NLM_F_DUMP, 7, 0) > 0);
	struct reply r;
	read_replies((int)fd, 7, &r);
	test_true(r.done);
	test_int_eq(r.error, 1);
	test_false(r.bad_pid);
	test_false(r.bad_src);
	test_false(r.lo_has_mac);

	android_off();  /* the host's own view, unhooked */
	struct ifaddrs *ifa = NULL;
	test_int_eq(getifaddrs(&ifa), 0);
	for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr) continue;
		unsigned idx = if_nametoindex(p->ifa_name);
		int seen = 0;
		for (int i = 0; i < r.n_links; i++)
			if ((unsigned)r.index[i] == idx &&
			    !strcmp(r.names[i], p->ifa_name))
				seen = 1;
		test_true(seen);
	}
	freeifaddrs(ifa);

	close((int)fd);
	th_teardown(&v);
}

/* Non-dump RTM_GETLINK by index: one NEWLINK plus the requested ack;
 * an unknown index is ENODEV. */
test(hosted_rtnl_getlink_by_index_synthesized)
{
	th_view v;
	th_setup(&v, "rtnl");
	unsigned lo = if_nametoindex("lo");
	test_true(lo > 0);
	android_on(1);
	long fd = nl_socket(test_ctx, SOCK_RAW);

	test_true(send_getlink(test_ctx, fd, NLM_F_ACK, 8, (int32_t)lo) > 0);
	struct reply r;
	read_replies((int)fd, 8, &r);
	test_int_eq(r.error, 0);
	test_int_eq(r.n_links, 1);
	test_str_eq(r.names[0], "lo");

	test_true(send_getlink(test_ctx, fd, 0, 9, 0x7ffffff0) > 0);
	read_replies((int)fd, 9, &r);
	test_int_eq(r.error, -ENODEV);

	close((int)fd);
	android_off();
	th_teardown(&v);
}

/* Requests the kernel allows are relayed, not synthesized: a GETLINK
 * where only bind is denied carries the real IFLA_ADDRESS; a GETADDR
 * dump sent libtorrent-style (plain send) arrives intact; a kernel
 * error comes back as the kernel's NLMSG_ERROR. */
test(hosted_rtnl_relay)
{
	th_view v;
	th_setup(&v, "rtnl");
	android_on(0);
	long fd = nl_socket(test_ctx, SOCK_DGRAM);
	test_true(tawcroot_rtnl_is_stub((int)fd));

	test_true(send_getlink(test_ctx, fd, NLM_F_DUMP, 10, 0) > 0);
	struct reply r;
	read_replies((int)fd, 10, &r);
	test_true(r.done);
	test_true(r.lo_has_mac);
	test_false(r.bad_pid);
	test_false(r.bad_src);

	struct {
		struct nlmsghdr  h;
		struct ifaddrmsg m;
	} rq;
	memset(&rq, 0, sizeof rq);
	rq.h.nlmsg_len = NLMSG_LENGTH(sizeof rq.m);
	rq.h.nlmsg_type = RTM_GETADDR;
	rq.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	rq.h.nlmsg_seq = 11;
	test_int_eq(th_sys(TAWC_SYS_sendto, fd, &rq, rq.h.nlmsg_len, 0, 0, 0),
		    (long)rq.h.nlmsg_len);
	read_replies((int)fd, 11, &r);
	test_true(r.done);
	test_int_eq(r.error, 1);
	test_true(r.n_other > 0);
	test_false(r.bad_pid);

	/* No ack asked for: a failure still comes back, a success would
	 * leave nothing behind (the forced ack is dropped). Under SELinux
	 * (Android) the send itself is denied, which passes through. */
	struct {
		struct nlmsghdr h;
		struct rtmsg    m;
	} del;
	memset(&del, 0, sizeof del);
	del.h.nlmsg_len = NLMSG_LENGTH(sizeof del.m);
	del.h.nlmsg_type = RTM_DELROUTE;
	del.h.nlmsg_flags = NLM_F_REQUEST;
	del.h.nlmsg_seq = 12;
	del.m.rtm_family = AF_INET;
	del.m.rtm_table = RT_TABLE_MAIN;
	struct iovec iov = { &del, del.h.nlmsg_len };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	long sent = th_sys(TAWC_SYS_sendmsg, fd, &mh, 0, 0, 0, 0);
	if (sent != -EACCES) {
		test_int_eq(sent, (long)del.h.nlmsg_len);
		read_replies((int)fd, 12, &r);
		test_true(r.error < 0);
	}

	close((int)fd);
	android_off();
	th_teardown(&v);
}
