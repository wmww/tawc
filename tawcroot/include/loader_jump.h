/* Per-arch entry trampoline. Switches SP to the synthesized initial
 * stack and jumps to the target entry point. Never returns.
 *
 * On entry the synthesized stack must already be 16-byte aligned at
 * `sp` and look exactly like a kernel-built initial stack (argc at
 * [sp+0], argv... at [sp+8], etc — see notes/tawcroot/path-translation.md and
 * loader_stack.h).
 *
 * `entry` is the address to jump to:
 *   - dynamic guests: the ld.so entry (placement.entry of ld.so).
 *   - static guests:  the binary's e_entry (placement.entry of binary).
 *
 * RDX (x86_64) / x0 (aarch64) is set to 0 — that's the rtld_fini
 * pointer the SysV ABI passes for "atexit" registration. glibc's
 * _start checks for non-NULL and registers it; passing 0 is correct
 * when there's no rtld_fini handler (ld.so does its own cleanup).
 *
 * Implementation lives in arch/<arch>_loader_jump.S so the function
 * has no prologue/epilogue to corrupt the new SP.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void tawc_loader_jump(uintptr_t sp, uintptr_t entry) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
