/* Bionic .so with an unresolved weak __thread reference. The static
 * linker emits a R_AARCH64_TLSDESC reloc against a weak undefined
 * symbol. With libhybris's loud-error guard at linker.cpp's
 * R_GENERIC_TLSDESC handler, hybris_dlopen of this .so now fails
 * outright (instead of silently installing tlsdesc_resolver_unresolved_weak
 * which reads raw tpidr_el0 / glibc's TP). The repro asserts the
 * expected dlopen failure + matching dlerror.
 *
 * Issues file (now resolved):
 *   issues/libhybris-tls-dlsym-and-weak-tlsdesc.md (#2). */
extern __thread int weak_unresolved_tls __attribute__((weak));
void* get_weak_tls_addr(void) { return &weak_unresolved_tls; }
