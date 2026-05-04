/* Fake-root identity surface — phase 1.
 *
 * Mirrors `proot -0`: getuid/geteuid/getgid/getegid (and resuid/resgid)
 * report 0 regardless of the actual host uid. See identity.c.
 */

#pragma once

/* Register the identity handler set in the dispatch table. Called from
 * tawcroot_dispatch_init. */
void tawcroot_identity_register(void);
