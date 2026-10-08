/* NETLINK_ROUTE emulation for the untrusted_app domain.
 *
 * Android 11+ denies apps (targetSdk >= 30) `bind()` on NETLINK_ROUTE
 * sockets and the RTM_GETLINK request, both with EACCES; RTM_GETADDR /
 * RTM_GETROUTE still work. That breaks interface enumeration for every
 * glibc program (getifaddrs, if_nameindex: Qt's QNetworkInterface,
 * Python, iproute2), Go's net.Interfaces, libtorrent (a failed bind
 * pauses its whole session) and Chromium's address tracker.
 *
 * When a one-time probe shows the kernel denies the bind, socket() for
 * NETLINK_ROUTE returns a stub instead: an AF_UNIX datagram socket
 * bound to an abstract name carrying TAWC_RTNL_TAG, so later syscalls
 * recognize it without fd bookkeeping (like the uevent stub in
 * syscalls_socket.c). bind/connect on the stub succeed; getsockname
 * reports a sockaddr_nl. Sends on it fail natively (ENOTCONN / EINVAL),
 * and only then does syscalls_socket.c hand the datagram here:
 *
 *   - each request is relayed through a private real netlink socket
 *     and the kernel's replies are copied to the stub, nlmsg_pid
 *     rewritten to the stub's port id;
 *   - an RTM_GETLINK the kernel refuses is synthesized from SIOCGIFNAME
 *     / SIOCGIFFLAGS / SIOCGIFMTU, which apps may still use. No
 *     IFLA_ADDRESS: Android hides MAC addresses from apps.
 *
 * Replies are queued before the send returns, so the guest's untrapped
 * recv/recvmsg/poll/epoll read them natively. They come from a writer
 * bound to a 10-byte abstract name that reads back as a sockaddr_nl
 * with nl_pid == 0 and nl_groups == 0 and the right length: glibc and
 * iproute2 drop replies whose source nl_pid isn't the kernel's, libmnl
 * checks msg_namelen. Only nl_family reads AF_UNIX.
 *
 * Not emulated: multicast groups (a stub never reports link/address
 * changes, like a network that never changes), SOL_NETLINK setsockopts
 * (untrapped; EOPNOTSUPP on the stub, which the consumers we know
 * treat as optional), and write()/writev() on the stub (untrapped). */

#include <stddef.h>
#include <stdint.h>

#include <linux/mman.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "errno_neg.h"
#include "io.h"
#include "raw_sys.h"
#include "rtnl_emu.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "tawc_uapi.h"
#include "usercopy.h"

#define AF_UNIX_FAM      1
#define AF_INET_FAM      2
#define AF_NETLINK_FAM   16
#define SOCK_DGRAM_T     2
#define SOCK_RAW_T       3
#define SOL_SOCKET_L     1
#define SO_SNDBUF_O      7
#define MSG_DONTWAIT_F   0x40

#define SIOCGIFNAME_IOC  0x8910
#define SIOCGIFFLAGS_IOC 0x8913
#define SIOCGIFMTU_IOC   0x8921

#define IFF_LOOPBACK_F    0x8
#define IFF_POINTOPOINT_F 0x10
#define IFF_RUNNING_F     0x40
#define IFF_LOWER_UP_F    0x10000

#define ARPHRD_ETHER_T    1
#define ARPHRD_LOOPBACK_T 772
#define ARPHRD_NONE_T     0xfffe

#define IF_OPER_UNKNOWN_S 0
#define IF_OPER_DOWN_S    2
#define IF_OPER_UP_S      6

#define TAWC_ENOBUFS  (-105)
#define TAWC_ENODEV   (-19)
#define TAWC_EMSGSIZE (-90)

#define TAWC_RTNL_TAG "tawcroot-rtnl."

/* Scratch: guest request, kernel reply, outgoing datagram. Mapped per
 * call — the handler stack is a few KiB and this path is rare. */
#define REQ_CAP   (32 * 1024)
#define RECV_CAP  (64 * 1024)
#define OUT_CAP   (32 * 1024)
#define SCRATCH_SIZE (REQ_CAP + RECV_CAP + OUT_CAP)

/* Datagram size for synthesized replies. The kernel's own dump skbs
 * are ~NLMSG_GOODSIZE (a page minus overhead); staying under that keeps
 * 4 KiB reader buffers (glibc's) from seeing MSG_TRUNC. */
#define OUT_DGRAM 3584

/* Probe this many indexes past the last interface found. Index gaps
 * come from deleted interfaces (VPN tun churn). */
#define IFINDEX_SLACK 32
#define IFINDEX_MAX   65536

/* NLMSG_OK / NLMSG_NEXT trip -Wsign-compare with an int length. */
#define MSG_OK(h, left) \
	((left) >= (int)sizeof(struct nlmsghdr) && \
	 (h)->nlmsg_len >= sizeof(struct nlmsghdr) && \
	 (h)->nlmsg_len <= (unsigned)(left))

struct un_addr {
	uint16_t fam;
	char     path[108];
};

struct ifreq_m {
	char name[16];
	union {
		int32_t  ivalue;
		uint16_t flags;
		char     pad[24];
	} u;
};

struct ctx {
	int            w;       /* writer socket */
	struct un_addr dst;     /* the stub's address */
	long           dst_len;
	uint32_t       port;
	int            priv;    /* private real netlink socket, or -1 */
	char          *req;
	char          *rbuf;
	char          *out;
	size_t         out_len;
	long           err;     /* first queueing failure */
};

int tawcroot_rtnl_is_stub_name(const void *addr, long len)
{
	const struct un_addr *un = addr;
	size_t tag = sizeof(TAWC_RTNL_TAG) - 1;
	if (un->fam != AF_UNIX_FAM) return 0;
	if (len < (long)(sizeof(uint16_t) + 1 + tag)) return 0;
	if (un->path[0] != '\0') return 0;
	return memcmp(un->path + 1, TAWC_RTNL_TAG, tag) == 0;
}

static long sock_name(int fd, struct un_addr *un)
{
	uint32_t len = sizeof *un;
	memset(un, 0, sizeof *un);
	long rv = TAWC_RAW(TAWC_SYS_getsockname, fd, (long)un, (long)&len,
			   0, 0, 0);
	return rv < 0 ? rv : (long)len;
}

int tawcroot_rtnl_is_stub(int fd)
{
	struct un_addr un;
	long len = sock_name(fd, &un);
	return len > 0 && tawcroot_rtnl_is_stub_name(&un, len);
}

unsigned int tawcroot_rtnl_port_id(void)
{
	return (unsigned int)tawc_getpid();
}

/* "\0tawcroot-rtnl.<pid>.<seq>"; returns the addrlen. */
static long stub_addr(struct un_addr *un, unsigned long pid,
		      unsigned long seq)
{
	memset(un, 0, sizeof *un);
	un->fam = AF_UNIX_FAM;
	size_t pos = 1;
	const char *tag = TAWC_RTNL_TAG;
	for (size_t i = 0; tag[i]; i++) un->path[pos++] = tag[i];
	unsigned long parts[2] = { pid, seq };
	for (int p = 0; p < 2; p++) {
		char tmp[21];
		int n = 0;
		unsigned long v = parts[p];
		do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
		while (n) un->path[pos++] = tmp[--n];
		if (p == 0) un->path[pos++] = '.';
	}
	return (long)(sizeof(uint16_t) + pos);
}

/* Writer for reply datagrams. Its abstract name is "\0<x>" plus eight
 * NULs: read back as a sockaddr_nl, nl_pid and nl_groups are 0 (the
 * kernel) and the length is 12. <x> only disambiguates concurrent
 * writers; each lives for one send. */
static long open_writer(void)
{
	long w = TAWC_RAW(TAWC_SYS_socket, AF_UNIX_FAM,
			  SOCK_DGRAM_T | O_CLOEXEC, 0, 0, 0, 0);
	if (w < 0) return w;
	int32_t sndbuf = 4 * 1024 * 1024;  /* clamped to wmem_max */
	TAWC_RAW(TAWC_SYS_setsockopt, w, SOL_SOCKET_L, SO_SNDBUF_O,
		 (long)&sndbuf, sizeof sndbuf, 0);
	unsigned long base = (unsigned long)tawc_getpid();
	for (unsigned long i = 0; i < 255; i++) {
		struct un_addr un;
		memset(&un, 0, sizeof un);
		un.fam = AF_UNIX_FAM;
		un.path[1] = (char)((base + i) % 255 + 1);
		long e = TAWC_RAW(TAWC_SYS_bind, w, (long)&un,
				  (long)(sizeof(uint16_t) + 10), 0, 0, 0);
		if (e == 0) return w;
		if (e != TAWC_EADDRINUSE) break;
	}
	tawc_close((int)w);
	return TAWC_ENOBUFS;
}

static void post(struct ctx *c, const void *buf, size_t len)
{
	if (c->err || !len) return;
	long rv = TAWC_RAW(TAWC_SYS_sendto, c->w, (long)buf, (long)len,
			   MSG_DONTWAIT_F, (long)&c->dst, c->dst_len);
	if (rv < 0) c->err = rv == TAWC_EAGAIN ? TAWC_ENOBUFS : rv;
}

static void flush(struct ctx *c)
{
	post(c, c->out, c->out_len);
	c->out_len = 0;
}

/* Room for one `len`-byte message in the pending datagram. */
static struct nlmsghdr *reserve(struct ctx *c, size_t len)
{
	len = NLMSG_ALIGN(len);
	if (c->out_len && c->out_len + len > OUT_DGRAM) flush(c);
	if (c->out_len + len > OUT_CAP) return 0;
	struct nlmsghdr *h = (struct nlmsghdr *)(c->out + c->out_len);
	memset(h, 0, len);
	c->out_len += len;
	return h;
}

static void put_hdr(struct ctx *c, struct nlmsghdr *h, size_t len,
		    uint16_t type, uint16_t flags, uint32_t seq)
{
	h->nlmsg_len = (uint32_t)len;
	h->nlmsg_type = type;
	h->nlmsg_flags = flags;
	h->nlmsg_seq = seq;
	h->nlmsg_pid = c->port;
}

/* NLMSG_ERROR for `req`. Like the kernel without NETLINK_CAP_ACK: an
 * error echoes the whole request, a success ack only its header. */
static void put_error(struct ctx *c, const struct nlmsghdr *req, int err)
{
	size_t echo = err ? req->nlmsg_len : sizeof *req;
	size_t len = NLMSG_LENGTH(sizeof(int32_t) + echo);
	struct nlmsghdr *h = reserve(c, len);
	if (!h) {
		c->err = TAWC_ENOBUFS;
		return;
	}
	put_hdr(c, h, len, NLMSG_ERROR, err ? 0 : NLM_F_CAPPED,
		req->nlmsg_seq);
	int32_t e = err;
	memcpy(NLMSG_DATA(h), &e, sizeof e);
	memcpy((char *)NLMSG_DATA(h) + sizeof e, req, echo);
}

static void put_done(struct ctx *c, const struct nlmsghdr *req)
{
	size_t len = NLMSG_LENGTH(sizeof(int32_t));
	struct nlmsghdr *h = reserve(c, len);
	if (!h) {
		c->err = TAWC_ENOBUFS;
		return;
	}
	put_hdr(c, h, len, NLMSG_DONE, NLM_F_MULTI, req->nlmsg_seq);
}

static struct rtattr *put_attr(struct rtattr *a, uint16_t type,
			       const void *data, size_t len)
{
	a->rta_type = type;
	a->rta_len = (uint16_t)RTA_LENGTH(len);
	memcpy(RTA_DATA(a), data, len);
	return (struct rtattr *)((char *)a + RTA_SPACE(len));
}

struct link {
	int32_t  index;
	uint32_t flags;
	int32_t  mtu;
	char     name[16];
};

static int probe_link(int s, int32_t index, struct link *l)
{
	struct ifreq_m ifr;
	memset(&ifr, 0, sizeof ifr);
	ifr.u.ivalue = index;
	if (TAWC_RAW(TAWC_SYS_ioctl, s, SIOCGIFNAME_IOC, (long)&ifr,
		     0, 0, 0) < 0)
		return 0;
	memset(l, 0, sizeof *l);
	l->index = index;
	memcpy(l->name, ifr.name, sizeof l->name - 1);
	if (TAWC_RAW(TAWC_SYS_ioctl, s, SIOCGIFFLAGS_IOC, (long)&ifr,
		     0, 0, 0) == 0)
		l->flags = ifr.u.flags;
	memset(&ifr.u, 0, sizeof ifr.u);
	if (TAWC_RAW(TAWC_SYS_ioctl, s, SIOCGIFMTU_IOC, (long)&ifr,
		     0, 0, 0) == 0)
		l->mtu = ifr.u.ivalue;
	/* SIOCGIFFLAGS is 16 bits wide; the kernel reports LOWER_UP with
	 * a carrier, which is what RUNNING says here. */
	if (l->flags & IFF_RUNNING_F) l->flags |= IFF_LOWER_UP_F;
	return 1;
}

static void put_link(struct ctx *c, const struct nlmsghdr *req,
		     uint16_t flags, const struct link *l)
{
	size_t name_len = strlen(l->name) + 1;
	size_t len = NLMSG_LENGTH(sizeof(struct ifinfomsg)) +
		     RTA_SPACE(name_len) + RTA_SPACE(sizeof(uint32_t)) +
		     RTA_SPACE(sizeof(uint8_t));
	struct nlmsghdr *h = reserve(c, len);
	if (!h) {
		c->err = TAWC_ENOBUFS;
		return;
	}
	put_hdr(c, h, len, RTM_NEWLINK, flags, req->nlmsg_seq);

	struct ifinfomsg *ifi = NLMSG_DATA(h);
	ifi->ifi_family = 0;
	ifi->ifi_index = l->index;
	ifi->ifi_flags = l->flags;
	uint8_t oper;
	if (l->flags & IFF_LOOPBACK_F) {
		ifi->ifi_type = ARPHRD_LOOPBACK_T;
		oper = IF_OPER_UNKNOWN_S;
	} else {
		ifi->ifi_type = (l->flags & IFF_POINTOPOINT_F)
			? ARPHRD_NONE_T : ARPHRD_ETHER_T;
		oper = (l->flags & IFF_RUNNING_F) ? IF_OPER_UP_S
						   : IF_OPER_DOWN_S;
	}

	struct rtattr *a = (struct rtattr *)((char *)ifi +
					     NLMSG_ALIGN(sizeof *ifi));
	a = put_attr(a, IFLA_IFNAME, l->name, name_len);
	uint32_t mtu = (uint32_t)l->mtu;
	a = put_attr(a, IFLA_MTU, &mtu, sizeof mtu);
	put_attr(a, IFLA_OPERSTATE, &oper, sizeof oper);
}

static long open_priv(struct ctx *c)
{
	if (c->priv >= 0) return 0;
	long s = TAWC_RAW(TAWC_SYS_socket, AF_NETLINK_FAM,
			  SOCK_RAW_T | O_CLOEXEC, NETLINK_ROUTE, 0, 0, 0);
	if (s < 0) return s;
	c->priv = (int)s;
	return 0;
}

static long priv_send(struct ctx *c, const void *msg, size_t len)
{
	struct sockaddr_nl k;
	memset(&k, 0, sizeof k);
	k.nl_family = AF_NETLINK_FAM;
	return TAWC_RAW(TAWC_SYS_sendto, c->priv, (long)msg, (long)len, 0,
			(long)&k, sizeof k);
}

static long priv_recv(struct ctx *c)
{
	for (;;) {
		long n = TAWC_RAW(TAWC_SYS_recvfrom, c->priv, (long)c->rbuf,
				  RECV_CAP, 0, 0, 0);
		if (n != TAWC_EINTR) return n;
	}
}

/* Highest ifindex holding an address, from a real RTM_GETADDR dump —
 * so interfaces past a large index gap still get probed. 0 when the
 * dump isn't available. */
static int32_t max_addr_index(struct ctx *c, uint32_t seq)
{
	if (open_priv(c) < 0) return 0;
	struct {
		struct nlmsghdr  h;
		struct ifaddrmsg m;
	} rq;
	memset(&rq, 0, sizeof rq);
	rq.h.nlmsg_len = NLMSG_LENGTH(sizeof rq.m);
	rq.h.nlmsg_type = RTM_GETADDR;
	rq.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	rq.h.nlmsg_seq = seq;
	if (priv_send(c, &rq, rq.h.nlmsg_len) < 0) return 0;

	int32_t max = 0;
	for (;;) {
		long n = priv_recv(c);
		if (n <= 0) return max;
		int left = (int)n;
		for (struct nlmsghdr *h = (struct nlmsghdr *)c->rbuf;
		     MSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_seq != seq) continue;
			if (h->nlmsg_type == NLMSG_DONE ||
			    h->nlmsg_type == NLMSG_ERROR)
				return max;
			if (h->nlmsg_type != RTM_NEWADDR ||
			    h->nlmsg_len < NLMSG_LENGTH(sizeof rq.m))
				continue;
			const struct ifaddrmsg *ifa = NLMSG_DATA(h);
			if ((int32_t)ifa->ifa_index > max)
				max = (int32_t)ifa->ifa_index;
		}
	}
}

/* The name a non-dump RTM_GETLINK asks for (IFLA_IFNAME), or NULL. */
static const char *req_ifname(const struct nlmsghdr *req)
{
	size_t off = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	if (req->nlmsg_len <= off) return 0;
	int left = (int)(req->nlmsg_len - NLMSG_ALIGN(off));
	for (const struct rtattr *a = (const struct rtattr *)
		     ((const char *)req + NLMSG_ALIGN(off));
	     RTA_OK(a, left); a = RTA_NEXT(a, left)) {
		if (a->rta_type != IFLA_IFNAME) continue;
		const char *s = RTA_DATA(a);
		size_t n = RTA_PAYLOAD(a);
		if (n && n <= 16 && s[n - 1] == '\0') return s;
	}
	return 0;
}

static void do_getlink(struct ctx *c, const struct nlmsghdr *req)
{
	int dump = (req->nlmsg_flags & NLM_F_DUMP) != 0;
	int32_t want_index = 0;
	const char *want_name = 0;
	if (!dump) {
		if (req->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
			put_error(c, req, TAWC_EINVAL);
			return;
		}
		want_index = ((const struct ifinfomsg *)NLMSG_DATA(req))
				     ->ifi_index;
		want_name = req_ifname(req);
		if (!want_index && !want_name) {
			put_error(c, req, TAWC_EINVAL);
			return;
		}
	}

	long s = TAWC_RAW(TAWC_SYS_socket, AF_INET_FAM,
			  SOCK_DGRAM_T | O_CLOEXEC, 0, 0, 0, 0);
	if (s < 0) {
		put_error(c, req, (int)s);
		return;
	}

	int found = 0;
	struct link l;
	if (want_index) {
		if (probe_link((int)s, want_index, &l) &&
		    (!want_name || tawc_streq(l.name, want_name))) {
			put_link(c, req, 0, &l);
			found = 1;
		}
	} else {
		int32_t limit = max_addr_index(c, req->nlmsg_seq) +
				IFINDEX_SLACK;
		for (int32_t i = 1; i <= limit && i <= IFINDEX_MAX; i++) {
			if (!probe_link((int)s, i, &l)) continue;
			if (i + IFINDEX_SLACK > limit)
				limit = i + IFINDEX_SLACK;
			if (dump) {
				put_link(c, req, NLM_F_MULTI, &l);
			} else if (tawc_streq(l.name, want_name)) {
				put_link(c, req, 0, &l);
				found = 1;
				break;
			}
		}
	}
	tawc_close((int)s);

	if (dump)
		put_done(c, req);
	else if (!found)
		put_error(c, req, TAWC_ENODEV);
	else if (req->nlmsg_flags & NLM_F_ACK)
		put_error(c, req, 0);
}

/* Pass one request to the kernel and queue its replies. Non-dump
 * requests get NLM_F_ACK forced on so we know when the kernel is done;
 * the forced success ack is dropped again. */
static long relay(struct ctx *c, const struct nlmsghdr *req)
{
	long e = open_priv(c);
	if (e < 0) return e;
	int dump = (req->nlmsg_flags & NLM_F_DUMP) != 0;
	int want_ack = (req->nlmsg_flags & NLM_F_ACK) != 0;

	struct nlmsghdr *rq = (struct nlmsghdr *)c->rbuf;
	memcpy(rq, req, req->nlmsg_len);
	if (!dump) rq->nlmsg_flags |= NLM_F_ACK;
	e = priv_send(c, rq, req->nlmsg_len);
	if ((e == TAWC_EACCES || e == TAWC_EPERM) &&
	    req->nlmsg_type == RTM_GETLINK) {
		do_getlink(c, req);
		return 0;
	}
	if (e < 0) return e;

	flush(c);
	for (;;) {
		long n = priv_recv(c);
		if (n < 0) return n;
		if (n == 0) return 0;
		int done = 0;
		size_t keep = 0;
		int left = (int)n;
		for (struct nlmsghdr *h = (struct nlmsghdr *)c->rbuf;
		     MSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_seq != req->nlmsg_seq) continue;
			if (h->nlmsg_type == NLMSG_ERROR) done = 1;
			if (dump && h->nlmsg_type == NLMSG_DONE) done = 1;
			if (!dump && !want_ack && h->nlmsg_type == NLMSG_ERROR &&
			    h->nlmsg_len >= NLMSG_LENGTH(sizeof(int32_t)) &&
			    *(const int32_t *)NLMSG_DATA(h) == 0)
				continue;
			h->nlmsg_pid = c->port;
			size_t len = NLMSG_ALIGN(h->nlmsg_len);
			if ((size_t)left < len) len = (size_t)left;
			memmove(c->rbuf + keep, h, len);
			keep += len;
		}
		post(c, c->rbuf, keep);
		if (done) return 0;
	}
}

long tawcroot_rtnl_send(int fd, const struct tawc_rtnl_iov *iov,
			size_t n_iov)
{
	struct ctx c;
	memset(&c, 0, sizeof c);
	c.priv = -1;
	c.w = -1;
	c.dst_len = sock_name(fd, &c.dst);
	if (c.dst_len < 0) return c.dst_len;
	c.port = tawcroot_rtnl_port_id();

	long map = TAWC_RAW(TAWC_SYS_mmap, 0, SCRATCH_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map < 0) return TAWC_ENOBUFS;
	c.req = (char *)map;
	c.rbuf = c.req + REQ_CAP;
	c.out = c.rbuf + RECV_CAP;

	long ret = 0;
	size_t len = 0;
	for (size_t i = 0; i < n_iov && ret == 0; i++) {
		if (iov[i].len > REQ_CAP - len) {
			ret = TAWC_EMSGSIZE;
			break;
		}
		if (tawc_copy_from_guest(c.req + len, iov[i].len,
					 (const void *)(uintptr_t)iov[i].base))
			ret = TAWC_EFAULT;
		len += iov[i].len;
	}

	if (ret == 0) {
		long w = open_writer();
		if (w < 0) ret = w;
		else c.w = (int)w;
	}

	if (ret == 0) {
		int left = (int)len;
		for (struct nlmsghdr *h = (struct nlmsghdr *)c.req;
		     MSG_OK(h, left) && !c.err; h = NLMSG_NEXT(h, left)) {
			long e = relay(&c, h);
			if (e < 0) {
				ret = e;
				break;
			}
		}
		flush(&c);
		if (ret == 0 && c.err) ret = c.err;
	}

	if (c.priv >= 0) tawc_close(c.priv);
	if (c.w >= 0) tawc_close(c.w);
	TAWC_RAW(TAWC_SYS_munmap, map, SCRATCH_SIZE, 0, 0, 0, 0);
	return ret < 0 ? ret : (long)len;
}

/* 0 unknown, 1 denied, 2 allowed. Per process image: a fork inherits
 * the answer, an exec probes again. */
static int rtnl_mode;

void tawcroot_rtnl_reset(void)
{
	__atomic_store_n(&rtnl_mode, 0, __ATOMIC_RELAXED);
}

int tawcroot_rtnl_denied(void)
{
	int mode = __atomic_load_n(&rtnl_mode, __ATOMIC_RELAXED);
	if (mode) return mode == 1;
	long s = TAWC_RAW(TAWC_SYS_socket, AF_NETLINK_FAM,
			  SOCK_RAW_T | O_CLOEXEC, NETLINK_ROUTE, 0, 0, 0);
	if (s < 0) return 0;  /* nothing to stand in for */
	struct sockaddr_nl nl;
	memset(&nl, 0, sizeof nl);
	nl.nl_family = AF_NETLINK_FAM;
	long e = TAWC_RAW(TAWC_SYS_bind, s, (long)&nl, sizeof nl, 0, 0, 0);
	tawc_close((int)s);
	mode = (e == TAWC_EACCES || e == TAWC_EPERM) ? 1 : 2;
	__atomic_store_n(&rtnl_mode, mode, __ATOMIC_RELAXED);
	return mode == 1;
}

long tawcroot_rtnl_open_stub(long type)
{
	long base = type & 0xf;
	if (base != SOCK_RAW_T && base != SOCK_DGRAM_T) return TAWC_EINVAL;
	long s = TAWC_RAW(TAWC_SYS_socket, AF_UNIX_FAM,
			  (type & ~0xfL) | SOCK_DGRAM_T, 0, 0, 0, 0);
	if (s < 0) return s;

	static unsigned long seq;
	long pid = tawc_getpid();
	long e = TAWC_EADDRINUSE;
	for (int attempt = 0; attempt < 64 && e == TAWC_EADDRINUSE;
	     attempt++) {
		struct un_addr un;
		long len = stub_addr(&un, (unsigned long)pid,
				     __atomic_fetch_add(&seq, 1,
							__ATOMIC_RELAXED));
		e = TAWC_RAW(TAWC_SYS_bind, s, (long)&un, len, 0, 0, 0);
	}
	if (e < 0) {
		tawc_close((int)s);
		return e;
	}
	return s;
}
