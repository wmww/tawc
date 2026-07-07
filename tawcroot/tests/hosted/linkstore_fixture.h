/* Shared store fixture for the two linkstore suites (test_linkstore.c,
 * test_linkstore_kill.c): one copy of the store-path wiring so a
 * layout or fixture change can't silently leave one suite testing a
 * stale shape. Follows the rootfs_helpers.h pattern — plain statics,
 * hosted-libc only. */

#pragma once

#include <stdio.h>

#include "hosted.h"
#include "../integration/rootfs_helpers.h"

#include "linkstore.h"

static char g_store[4300];

/* Point the linkstore at a per-test sibling dir of the rootfs.
 * LATENT: created on the first emulated link. */
static inline void store_setup(th_view *v)
{
	snprintf(g_store, sizeof g_store, "%s-store", v->root);
	rh_rmrf(g_store);
	tawcroot_linkstore_configure(g_store);
}

static inline void store_teardown(void)
{
	tawcroot_linkstore_configure(NULL);
	rh_rmrf(g_store);
}
