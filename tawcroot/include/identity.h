/* Stateful virtual identity — tracked fake uid/gid.
 *
 * Replaces the phase-1 stateless fake root (get*id → 0, set*id → 0)
 * with a process-wide tracked identity, the way proot's fake_id0
 * extension works. Daemons that drop privileges and *verify the drop*
 * (sshd's permanently_set_uid) see rule-accurate Linux semantics:
 * after a drop, restoring the old ids fails with EPERM and the getters
 * report the target user. Identity is cosmetic-consistent only — file
 * access remains whatever the app uid can do. See identity.c and
 * notes/tawcroot/path-translation.md "Virtual identity and metadata".
 */

#pragma once

#include <stdint.h>

/* Supplementary-group shadow capacity. sshd/su set a handful; 32
 * matches typical NGROUPS usage with plenty of slack. setgroups
 * beyond this returns -ENOMEM (kernel-plausible). */
#define TAWC_IDENTITY_NGROUPS 32

typedef struct {
	uint32_t ruid, euid, suid, fsuid;
	uint32_t rgid, egid, sgid, fsgid;
	uint32_t ngroups;
	uint32_t groups[TAWC_IDENTITY_NGROUPS];
} tawc_identity;

/* Register the identity handler set in the dispatch table and reset
 * the state to root defaults (all ids 0, groups = {0}). Called from
 * tawcroot_dispatch_init. */
void tawcroot_identity_register(void);

/* Reset to root defaults. Test hook + register() helper. */
void tawcroot_identity_reset(void);

/* Torn-free snapshot of the current identity (seqlock read). */
void tawcroot_identity_get(tawc_identity *out);

/* Replace the whole identity (seqlock write). Used by --exec-child to
 * restore the pre-exec identity from exec_state. */
void tawcroot_identity_load(const tawc_identity *in);

/* Convenience: current virtual euid. The privilege predicate used by
 * the fake-decoration paths is `euid == 0` (fake root has all caps,
 * everyone else none). */
uint32_t tawcroot_identity_euid(void);
