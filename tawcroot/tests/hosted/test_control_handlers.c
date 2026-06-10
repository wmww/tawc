/* Hosted handler-level tests for syscalls_control.c. */

#include <cleat/test.h>

#include <unistd.h>

#include "hosted.h"

#include "errno_neg.h"
#include "sysnr.h"

#if defined(__x86_64__)
/* Legacy getpgrp routes to getpgid(0) — Android's untrusted_app filter
 * RET_TRAPs the x86_64-only getpgrp number, and the -ENOSYS
 * fallthrough used to break bash's job-control init on the in-app
 * terminal's pty. */
test(hosted_getpgrp_routes_to_getpgid)
{
	th_view v;
	th_setup(&v, "ctl-getpgrp");

	test_int_eq(th_sys(TAWC_SYS_getpgrp, 0, 0, 0, 0, 0, 0),
		    (long)getpgid(0));

	th_teardown(&v);
}
#endif
