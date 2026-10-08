/* Prod-env probe: interface enumeration must work at the app uid,
 * where Android denies rtnetlink bind() and RTM_GETLINK (rtnl_emu.c)
 * and allows SIOCGIFNAME only on inet sockets (syscalls_fd.c).
 *
 * Replays the sequences real consumers use:
 *   - glibc getifaddrs: socket, bind, getsockname, sendto(kernel)
 *     RTM_GETLINK dump, recvmsg checking the source is the kernel and
 *     replies carry our port id;
 *   - libtorrent: plain send() of an RTM_GETADDR dump;
 *   - glibc if_indextoname: SIOCGIFNAME on an AF_UNIX socket.
 * Exit 42 iff all three see "lo"; otherwise print why and exit 1. */

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static char buf[65536];

/* Read one dump; return the number of `type` messages, -1 on error.
 * *saw_lo is set when an RTM_NEWLINK names "lo" or an RTM_NEWADDR is
 * on lo's index. */
static int read_dump(int fd, unsigned seq, unsigned port, int type,
		     int *saw_lo)
{
	int n_msgs = 0;
	for (;;) {
		struct sockaddr_nl src;
		struct iovec iov = { buf, sizeof buf };
		struct msghdr mh = { .msg_name = &src,
				     .msg_namelen = sizeof src,
				     .msg_iov = &iov, .msg_iovlen = 1 };
		ssize_t n = recvmsg(fd, &mh, 0);
		if (n <= 0) {
			perror("recvmsg");
			return -1;
		}
		if (mh.msg_namelen != sizeof src || src.nl_pid != 0) {
			printf("reply not from the kernel (len %u pid %u)\n",
			       mh.msg_namelen, src.nl_pid);
			return -1;
		}
		int left = (int)n;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
		     NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_seq != seq || h->nlmsg_pid != port) {
				printf("reply seq %u pid %u, want %u %u\n",
				       h->nlmsg_seq, h->nlmsg_pid, seq, port);
				return -1;
			}
			if (h->nlmsg_type == NLMSG_DONE) return n_msgs;
			if (h->nlmsg_type == NLMSG_ERROR) {
				printf("NLMSG_ERROR %d\n",
				       ((struct nlmsgerr *)NLMSG_DATA(h))->error);
				return -1;
			}
			if (h->nlmsg_type != type) continue;
			n_msgs++;
			if (type == RTM_NEWADDR) {
				struct ifaddrmsg *ifa = NLMSG_DATA(h);
				if (ifa->ifa_index == if_nametoindex("lo"))
					*saw_lo = 1;
				continue;
			}
			struct ifinfomsg *ifi = NLMSG_DATA(h);
			int al = (int)IFLA_PAYLOAD(h);
			for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, al);
			     a = RTA_NEXT(a, al))
				if (a->rta_type == IFLA_IFNAME &&
				    !strcmp(RTA_DATA(a), "lo"))
					*saw_lo = 1;
		}
	}
}

static int send_dump(int fd, int type, unsigned seq, int with_addr)
{
	struct {
		struct nlmsghdr  h;
		struct ifinfomsg m;
	} rq;
	memset(&rq, 0, sizeof rq);
	rq.h.nlmsg_len = NLMSG_LENGTH(sizeof rq.m);
	rq.h.nlmsg_type = type;
	rq.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	rq.h.nlmsg_seq = seq;
	struct sockaddr_nl k = { .nl_family = AF_NETLINK };
	ssize_t n = with_addr
		? sendto(fd, &rq, rq.h.nlmsg_len, 0, (struct sockaddr *)&k,
			 sizeof k)
		: send(fd, &rq, rq.h.nlmsg_len, 0);
	if (n != (ssize_t)rq.h.nlmsg_len) {
		perror("send");
		return -1;
	}
	return 0;
}

int main(void)
{
	int ok = 1;

	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	struct sockaddr_nl me = { .nl_family = AF_NETLINK };
	socklen_t len = sizeof me;
	if (fd < 0 || bind(fd, (struct sockaddr *)&me, sizeof me) < 0 ||
	    getsockname(fd, (struct sockaddr *)&me, &len) < 0) {
		perror("netlink socket/bind/getsockname");
		return 1;
	}
	int lo = 0;
	int n = send_dump(fd, RTM_GETLINK, 1, 1) ? -1
		: read_dump(fd, 1, me.nl_pid, RTM_NEWLINK, &lo);
	printf("RTM_GETLINK: %d links, lo %s\n", n, lo ? "seen" : "missing");
	ok &= n > 0 && lo;

	lo = 0;
	n = send_dump(fd, RTM_GETADDR, 2, 0) ? -1
		: read_dump(fd, 2, me.nl_pid, RTM_NEWADDR, &lo);
	printf("RTM_GETADDR: %d addrs, lo %s\n", n, lo ? "seen" : "missing");
	ok &= n > 0 && lo;
	close(fd);

	int us = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	ifr.ifr_ifindex = (int)if_nametoindex("lo");
	if (us < 0 || ioctl(us, SIOCGIFNAME, &ifr) < 0) {
		perror("SIOCGIFNAME on AF_UNIX");
		ok = 0;
	} else {
		printf("SIOCGIFNAME(%d) on AF_UNIX: %s\n", ifr.ifr_ifindex,
		       ifr.ifr_name);
		ok &= !strcmp(ifr.ifr_name, "lo");
	}
	return ok ? 42 : 1;
}
