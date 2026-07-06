/* --exec-child entry point. See notes/tawcroot/seccomp-filter.md "Approach A: re-exec
 * into ourselves first" and src/child.c. */

#pragma once

int tawcroot_child_main(int argc, char **argv);
