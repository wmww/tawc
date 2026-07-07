/* Negated-errno constants for tawcroot return values.
 *
 * tawcroot is freestanding and never links libc, so it can't pull
 * `<errno.h>` (and the values it would expose are positive — wrong
 * sign for our convention). Handlers and helpers return raw `-errno`
 * directly, the same shape the kernel hands back through SYSCALL_RET.
 *
 * Convention enforced here: every constant in this header is the
 * NEGATED value (`TAWC_EINVAL == -22`). Call sites read as
 *
 *     return TAWC_EINVAL;
 *
 * not `-TAWC_EINVAL`. This is the single source of truth — do not
 * redefine any of these inline. Add new entries here as new errnos
 * become callers' return values.
 *
 * The Linux ABI fixes these numbers across both arches we ship to,
 * so the literals are portable as written. */

#pragma once

#define TAWC_EPERM         (-1)
#define TAWC_ENOENT        (-2)
#define TAWC_EINTR         (-4)
#define TAWC_E2BIG         (-7)
#define TAWC_ENOEXEC       (-8)
#define TAWC_EIO           (-5)
#define TAWC_EBADF         (-9)
#define TAWC_EAGAIN        (-11)
#define TAWC_ENOMEM        (-12)
#define TAWC_EACCES        (-13)
#define TAWC_EFAULT        (-14)
#define TAWC_EBUSY         (-16)
#define TAWC_EEXIST        (-17)
#define TAWC_EXDEV         (-18)
#define TAWC_ENOTDIR       (-20)
#define TAWC_EISDIR        (-21)
#define TAWC_EINVAL        (-22)
#define TAWC_EMFILE        (-24)
#define TAWC_ENOSPC        (-28)
#define TAWC_ERANGE        (-34)
#define TAWC_ENAMETOOLONG  (-36)
#define TAWC_ENOSYS        (-38)
#define TAWC_ELOOP         (-40)
#define TAWC_ENODATA       (-61)
#define TAWC_EOPNOTSUPP    (-95)
#define TAWC_EPROTONOSUPPORT (-93)

/* Compile-time guard against the previous positive-valued convention
 * silently coming back. If a future edit defines `TAWC_EINVAL` as `22`,
 * every existing `return TAWC_EINVAL;` would silently become a successful
 * positive return. */
_Static_assert(TAWC_EINVAL < 0, "errno_neg.h constants must be negative");
