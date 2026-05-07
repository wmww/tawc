# tawcroot host build — incremental compilation for the inner loop.
#
# Builds three artifacts under build/tawcroot-host/:
#   - tawcroot           production binary, -static -nostdlib -no-pie -ffreestanding
#   - tawcroot-testhost  same source set + tawcroot/tests/testhost/src/, -DTAWCROOT_TESTHOST
#   - tests              cleat-driven orchestrator, hosted glibc, links cleat + STC
#
# Header deps tracked via gcc -MMD -MP, so a header edit rebuilds exactly the
# .c files that include it. Source list is explicit (no wildcards on
# tawcroot/src/); test sources auto-discovered with `find` so dropping a
# new .c into tawcroot/tests/{unit,handler,integration}/ Just Works on
# the next make.
#
# Cross-ABI NDK builds (aarch64/x86_64) live in tawcroot/build's bash flow;
# this Makefile is host-only. cleat must already be cloned at $(CLEAT) —
# tawcroot/build --abi=host handles that before invoking make.
#
# Usage:
#   make                    # everything
#   make tawcroot
#   make tawcroot-testhost
#   make tests
#   make clean
#   make -j                 # parallel (~3-5× faster on cold rebuild)

REPO     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
SRC_T    := $(REPO)/tawcroot
SRC_TH   := $(SRC_T)/tests/testhost
SRC_TS   := $(SRC_T)/tests
CLEAT    := $(REPO)/deps/cleat
OUT      := $(REPO)/build/tawcroot-host
OBJ      := $(OUT)/.obj

CC       ?= cc

# Host base address — same constraints as the x86_64 cross-build, pinned 256 MB
# lower to avoid colliding with anything glibc's loader might map. See
# notes/tawcroot.md "Why non-PIE" for the cross-build constraints.
HOST_BASE := 0x10000000

# --- flags ---

# tawcroot production / testhost: freestanding, no libc, fixed-base non-PIE.
# See tawcroot/build's COMMON_CFLAGS for the full reasoning.
TAWC_CFLAGS := \
    -O2 -fno-strict-aliasing -fno-stack-protector -fvisibility=hidden \
    -fno-exceptions -fno-asynchronous-unwind-tables -ffreestanding \
    -Wall -Wextra -Werror -D_GNU_SOURCE \
    -I$(SRC_T)/include \
    -MMD -MP

TAWC_LDFLAGS := \
    -static -nostdlib -nostartfiles \
    -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-z,relro \
    -Wl,--image-base=$(HOST_BASE)

TESTHOST_EXTRA_CFLAGS := -DTAWCROOT_TESTHOST=1 -I$(SRC_TH)/include

# cleat orchestrator: hosted glibc, C23 (cleat headers use nullptr /
# [[maybe_unused]]). Warning set mirrors deps/cleat/meson.build's cleat_c_args so
# the test build stays clean against cleat + STC's intentional unused-* /
# format-zero-length patterns.
TESTS_CFLAGS := \
    -std=c23 -O2 -fno-strict-aliasing \
    -fno-tree-loop-distribute-patterns \
    -Wno-missing-braces -Wno-unused-function -Wno-format-zero-length \
    -Wno-error=unused-variable -Wno-error=unused-parameter \
    -Wno-error=unused-function -Wno-error=unused-but-set-variable \
    -Wno-error=format-zero-length \
    -Werror=implicit-function-declaration \
    -Werror=return-type -Werror=incompatible-pointer-types \
    -I$(CLEAT)/include -I$(CLEAT)/include/cleat -I$(CLEAT)/stc/include \
    -I$(SRC_T)/include \
    -ffile-prefix-map=$(REPO)/= \
    -DTAWCROOT_TESTHOST_BIN=\"$(OUT)/tawcroot-testhost\" \
    -DTAWCROOT_PROD_BIN=\"$(OUT)/tawcroot\" \
    -DTAWCROOT_PROD_OBJ_DIR=\"$(OBJ)/prod/tawcroot/src\" \
    -DTAWCROOT_STATIC_EXIT42_BIN=\"$(OUT)/programs/static_exit42\" \
    -DTAWCROOT_STATIC_ARGC_RANDOM_BIN=\"$(OUT)/programs/static_argc_random\" \
    -DTAWCROOT_DYNAMIC_EXIT42_BIN=\"$(OUT)/programs/dynamic_exit42\" \
    -DTAWCROOT_DYNAMIC_ARGV_CHECK_BIN=\"$(OUT)/programs/dynamic_argv_check\" \
    -DTAWCROOT_STATIC_EXECVE_EXIT42_BIN=\"$(OUT)/programs/static_execve_exit42\" \
    -DTAWCROOT_STATIC_OPEN_CREAT_ARGV1_BIN=\"$(OUT)/programs/static_open_creat_argv1\" \
    -DTAWCROOT_DYNAMIC_BIG_STACK_BIN=\"$(OUT)/programs/dynamic_big_stack\" \
    -DTAWCROOT_ANDROID_FILTER_WRAP=\"$(OUT)/androidfilter/wrap\" \
    -DTAWCROOT_TEST_TMPDIR=\"/tmp\" \
    -MMD -MP

TESTS_LDFLAGS := -lm -pthread

# --- source lists ---

PROD_C := \
    $(SRC_T)/src/main.c \
    $(SRC_T)/src/io.c \
    $(SRC_T)/src/strings.c \
    $(SRC_T)/src/filter.c \
    $(SRC_T)/src/filter_build.c \
    $(SRC_T)/src/handler.c \
    $(SRC_T)/src/dispatch.c \
    $(SRC_T)/src/path.c \
    $(SRC_T)/src/path_fold.c \
    $(SRC_T)/src/path_orchestrate.c \
    $(SRC_T)/src/path_resolve.c \
    $(SRC_T)/src/proc_rewrite.c \
    $(SRC_T)/src/identity.c \
    $(SRC_T)/src/syscalls_fs.c \
    $(SRC_T)/src/syscalls_fd.c \
    $(SRC_T)/src/dirent_filter.c \
    $(SRC_T)/src/syscalls_control.c \
    $(SRC_T)/src/shm.c \
    $(SRC_T)/src/signal_shadow.c \
    $(SRC_T)/src/syscalls_socket.c \
    $(SRC_T)/src/usercopy.c \
    $(SRC_T)/src/loader_elf.c \
    $(SRC_T)/src/loader_map.c \
    $(SRC_T)/src/loader_stack.c \
    $(SRC_T)/src/loader_io_prod.c \
    $(SRC_T)/src/loader_exec.c \
    $(SRC_T)/src/exec_state.c \
    $(SRC_T)/src/exec_handler.c \
    $(SRC_T)/src/syscalls_exec.c

# Pure tawcroot sources also compiled into the cleat test orchestrator under
# hosted glibc, so unit tests can call them directly without re-execing the
# testhost. Must contain no raw-syscall calls (those would clash with glibc's
# `_start` and friends). `strings.c` is the canonical one — it intentionally
# has no `tawc_write` etc.
PROD_C_FOR_TESTS := \
    $(SRC_T)/src/strings.c \
    $(SRC_T)/src/filter_build.c \
    $(SRC_T)/src/path_fold.c \
    $(SRC_T)/src/path_orchestrate.c \
    $(SRC_T)/src/path_resolve.c \
    $(SRC_T)/src/proc_rewrite.c \
    $(SRC_T)/src/loader_elf.c \
    $(SRC_T)/src/loader_map.c \
    $(SRC_T)/src/loader_stack.c \
    $(SRC_T)/src/exec_state.c \
    $(SRC_T)/src/signal_shadow.c \
    $(SRC_T)/src/dirent_filter.c

TESTHOST_EXTRA_C := \
    $(SRC_TH)/src/testhost_main.c \
    $(SRC_TH)/src/smoke.c \
    $(SRC_TH)/src/child.c \
    $(SRC_TH)/src/phase1.c

ARCH_S := $(SRC_T)/src/arch/x86_64_stub.S \
          $(SRC_T)/src/arch/x86_64_loader_jump.S

# Asm files linked into the cleat test orchestrator. _start /
# tawcroot_raw_syscall must NOT be in here (clashes with glibc) — only
# pure helper trampolines that use no globals are safe. Currently just
# the loader entry trampoline so unit tests can exec it directly.
TESTS_ARCH_S := $(SRC_T)/src/arch/x86_64_loader_jump.S

# Cleat + STC source set, lifted from deps/cleat/meson.build (cleat_test pulls
# cleat_dep + stc_dep). Keep in sync when bumping the pinned cleat commit
# in tawcroot/build.
CLEAT_C := \
    $(CLEAT)/stc/src/fmt.c \
    $(CLEAT)/stc/src/cstr_utf8.c \
    $(CLEAT)/stc/src/cstr_core.c \
    $(CLEAT)/stc/src/random.c \
    $(CLEAT)/stc/src/cstr_io.c \
    $(CLEAT)/stc/src/stc_core.c \
    $(CLEAT)/stc/src/cspan.c \
    $(CLEAT)/stc/src/cregex.c \
    $(CLEAT)/stc/src/csview.c \
    $(CLEAT)/src/string_helpers.c \
    $(CLEAT)/src/panic.c \
    $(CLEAT)/src/filesystem.c \
    $(CLEAT)/src/real_filesystem.c \
    $(CLEAT)/src/subproc.c \
    $(CLEAT)/src/display.c \
    $(CLEAT)/src/failable.c \
    $(CLEAT)/src/base64.c \
    $(CLEAT)/src/random.c \
    $(CLEAT)/src/seri.c \
    $(CLEAT)/src/seri_json.c \
    $(CLEAT)/src/seri_number_parsing.c \
    $(CLEAT)/src/test.c \
    $(CLEAT)/src/default_test_runner.c \
    $(CLEAT)/src/stdout_test_output.c \
    $(CLEAT)/src/test_docs_code.c

# Test sources: every .c directly under tawcroot/tests/{unit,handler,integration}/.
# Subdirectories (e.g. tawcroot/tests/integration/programs/) are excluded
# so guest fixture programs with their own main() don't get linked into
# the cleat orchestrator. New cases land by dropping a .c file in one
# of those dirs — no list to maintain.
TEST_C := $(shell find $(SRC_TS)/unit $(SRC_TS)/handler $(SRC_TS)/integration \
                       -maxdepth 1 -name '*.c' -type f 2>/dev/null | sort)

# --- object-file paths ---
#
# Encoding: <obj_root>/<repo-relative source path>.o
#   $(REPO)/tawcroot/src/main.c -> $(OBJ)/prod/tawcroot/src/main.c.o
#   $(REPO)/deps/cleat/stc/src/random.c -> $(OBJ)/tests/cleat/stc/src/random.c.o
#
# Preserves directory structure under each obj_root, which naturally
# disambiguates same-basename files (cleat has stc/src/random.c AND
# src/random.c — both end up in distinct paths under $(OBJ)/tests/cleat/).

obj_path = $(addprefix $(1)/,$(addsuffix .o,$(patsubst $(REPO)/%,%,$(2))))

PROD_OBJS     := $(call obj_path,$(OBJ)/prod,    $(PROD_C) $(ARCH_S))
TESTHOST_OBJS := $(call obj_path,$(OBJ)/testhost,$(PROD_C) $(TESTHOST_EXTRA_C) $(ARCH_S))
TESTS_OBJS    := $(call obj_path,$(OBJ)/tests,   $(CLEAT_C) $(TEST_C) $(PROD_C_FOR_TESTS) $(TESTS_ARCH_S))

# --- targets ---

# Make's incremental build doesn't notice when a source file is *removed*:
# the orphan .o stays around and the link step picks it up. Catch this by
# pruning any obj path under $(OBJ) that no longer corresponds to a file in
# our current source lists. Cheap (handful of files); safe (only touches
# paths whose .c counterpart we own).
ALL_TRACKED_OBJS := $(PROD_OBJS) $(TESTHOST_OBJS) $(TESTS_OBJS)
EXISTING_OBJS    := $(shell find $(OBJ) -name '*.o' 2>/dev/null)
ORPHAN_OBJS      := $(filter-out $(ALL_TRACKED_OBJS),$(EXISTING_OBJS))
ifneq ($(strip $(ORPHAN_OBJS)),)
$(info pruning orphan .o files: $(ORPHAN_OBJS))
$(shell rm -f $(ORPHAN_OBJS) $(ORPHAN_OBJS:.o=.d))
# Also force a relink: Make won't otherwise notice that one of the binaries'
# input set shrank. Cheap — link is sub-second.
$(shell rm -f $(OUT)/tawcroot $(OUT)/tawcroot-testhost $(OUT)/tests)
endif

.PHONY: all tawcroot tawcroot-testhost tests fixtures clean
all: tawcroot tawcroot-testhost tests fixtures

tawcroot:           $(OUT)/tawcroot
tawcroot-testhost:  $(OUT)/tawcroot-testhost
tests:              $(OUT)/tests
fixtures:           $(OUT)/programs/static_exit42 \
                    $(OUT)/programs/static_argc_random \
                    $(OUT)/programs/dynamic_exit42 \
                    $(OUT)/programs/dynamic_argv_check \
                    $(OUT)/programs/static_execve_exit42 \
                    $(OUT)/programs/static_open_creat_argv1 \
                    $(OUT)/programs/dynamic_big_stack \
                    $(OUT)/androidfilter/wrap

clean:
	rm -rf $(OUT)

# --- test fixtures (tiny guest binaries the smoke tests load) ---

# static_exit42: minimal x86_64 ET_EXEC that does exit_group(42).
# Built `-static -nostdlib -no-pie` so it loads at the default base
# (0x400000) with no PT_INTERP. The smoke test in
# tawcroot/tests/unit/test_loader_smoke.c forks and uses our manual loader
# to map+jump it; success = the parent waits and sees exit code 42.
$(OUT)/programs/static_exit42: $(SRC_TS)/integration/programs/static_exit42_x86_64.S | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -static -nostdlib -no-pie -O2 -Wl,--build-id=none $< -o $@

# static_argc_random: same shape as static_exit42 but actually reads
# its initial stack — argc, argv[0], envp[0], and the AT_RANDOM auxv
# entry. The exit code encodes which step failed (or, on success,
# random16[0] & 0x7f + 100). Stronger validation of the stack synth.
$(OUT)/programs/static_argc_random: $(SRC_TS)/integration/programs/static_argc_random_x86_64.S | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -static -nostdlib -no-pie -O2 -Wl,--build-id=none $< -o $@

# dynamic_exit42: ET_DYN linked against the host's glibc. The phase
# 2.5 smoke loads both this binary AND its PT_INTERP-pointed ld.so,
# synthesizes the right auxv, and jumps to ld.so. Success = ld.so ran
# the C startup which called main() which returned 42.
$(OUT)/programs/dynamic_exit42: $(SRC_TS)/integration/programs/dynamic_exit42.c | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wl,--build-id=none $< -o $@

# dynamic_argv_check: validates that argc/argv/envp survive ld.so +
# __libc_start_main and reach main() intact. Distinct exit codes for
# each failure mode let smoke output bisect what broke.
$(OUT)/programs/dynamic_argv_check: $(SRC_TS)/integration/programs/dynamic_argv_check.c | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wl,--build-id=none $< -o $@

# static_execve_exit42: a static fixture that issues execve("/bin/static_exit42")
# from inside its own _start. Used by the phase 2g multi-process smoke
# (test_prod_rootfs.c::prod_rootfs_guest_does_execve) — the handler
# intercepts the execve, re-execs tawcroot with --exec-child, and the
# new process re-establishes rootfs view before manual-loading the
# target binary.
$(OUT)/programs/static_execve_exit42: $(SRC_TS)/integration/programs/static_execve_exit42_x86_64.S | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -static -nostdlib -no-pie -O2 -Wl,--build-id=none $< -o $@

# static_open_creat_argv1: minimal x86_64 fixture that opens argv[1]
# with O_WRONLY|O_CREAT|O_TRUNC and exits 0. The orphan-bypass test
# in test_exec_child.c uses this as the side-effect-bearing guest
# binary: an orphaned grandchild can't be waitpid'd, so we observe
# success by polling for the file the fixture creates.
$(OUT)/programs/static_open_creat_argv1: $(SRC_TS)/integration/programs/static_open_creat_argv1_x86_64.S | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -static -nostdlib -no-pie -O2 -Wl,--build-id=none $< -o $@

# androidfilter/wrap: host glibc binary that installs a synthesized
# Android-`untrusted_app`-shape seccomp BPF filter and execs its argv
# tail. Exists so test_androidfilter.c can run the existing handler
# suite under the same filter shape we'd see on a real device under
# run-as. Hosted glibc — not part of either tawcroot binary, only
# launched by the test orchestrator. See the file's header comment for
# the trapped-NR set and the deliberate-not-trapped exclusions.
$(OUT)/androidfilter/wrap: $(SRC_TS)/handler/androidfilter/wrap.c | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Werror -Wl,--build-id=none $< -o $@

# dynamic_big_stack: ET_DYN with a function that allocates a 380 KiB
# buffer in its stack frame and walks every page. Compiled with
# `-fstack-clash-protection` so GCC emits the page-by-page probe loop
# (`orq $0, (%rsp); cmp; jne`). Larger than the original 256 KiB
# loader stack; the regression test (`test_prod_exec.c::
# prod_exec_dynamic_big_stack`) loads it via `tawcroot --exec` and
# fails with SIGSEGV (exit ~139) if the loader stack is undersized,
# returns 42 if everything fits. See notes/tawcroot.md "wc segfault"
# for the original bug.
$(OUT)/programs/dynamic_big_stack: $(SRC_TS)/integration/programs/dynamic_big_stack.c | $(OUT)
	@mkdir -p $(dir $@)
	$(CC) -O2 -fstack-clash-protection -Wl,--build-id=none $< -o $@

# --- link rules ---

$(OUT)/tawcroot: $(PROD_OBJS) | $(OUT)
	$(CC) $(TAWC_LDFLAGS) -Wl,-soname,tawcroot $^ -o $@

$(OUT)/tawcroot-testhost: $(TESTHOST_OBJS) | $(OUT)
	$(CC) $(TAWC_LDFLAGS) -Wl,-soname,tawcroot-testhost $^ -o $@

$(OUT)/tests: $(TESTS_OBJS) | $(OUT)
	$(CC) $^ $(TESTS_LDFLAGS) -o $@

$(OUT):
	mkdir -p $@

# --- compile rules ---
#
# One pattern rule per (obj_root, language) pair. Pattern rules match on the
# `%` after the prefix, which captures the repo-relative source path.

$(OBJ)/prod/%.c.o: $(REPO)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TAWC_CFLAGS) -c $< -o $@

$(OBJ)/prod/%.S.o: $(REPO)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(TAWC_CFLAGS) -c $< -o $@

$(OBJ)/testhost/%.c.o: $(REPO)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TAWC_CFLAGS) $(TESTHOST_EXTRA_CFLAGS) -c $< -o $@

$(OBJ)/testhost/%.S.o: $(REPO)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(TAWC_CFLAGS) -c $< -o $@

$(OBJ)/tests/%.c.o: $(REPO)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TESTS_CFLAGS) -c $< -o $@

$(OBJ)/tests/%.S.o: $(REPO)/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

# --- header dep tracking ---

ALL_DEPS := $(PROD_OBJS:.o=.d) $(TESTHOST_OBJS:.o=.d) $(TESTS_OBJS:.o=.d)
-include $(ALL_DEPS)
