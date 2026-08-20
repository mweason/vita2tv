#ifndef DIAG_H
#define DIAG_H

/*
 * Minimal, temporary startup diagnostics for the kernel plugin.
 *
 * Deliberately does NOT use newlib (no snprintf/strlen/memcpy): pulling libc
 * into a kernel module makes the linker resolve sceClib* against the *user*
 * library SceLibKernel, which cannot be resolved in kernel space and makes the
 * whole .skprx fail to load. Everything here is hand-rolled and only uses
 * SceIofilemgrForDriver.
 */

#ifdef DIAG

void diag_reset(void);
void diag_step(const char *tag, int code);
void diag_stepn(const char *tag, const int *vals, int n);

#define DIAG_STEP(tag, code)	diag_step((tag), (int)(code))
#define DIAG_STEPN(tag, ...)	do { \
					const int __diag_v[] = {__VA_ARGS__}; \
					diag_stepn((tag), __diag_v, \
						(int)(sizeof(__diag_v) / sizeof(__diag_v[0]))); \
				} while (0)
#define DIAG_RESET()		diag_reset()

#else

#define DIAG_STEP(tag, code)	((void)0)
#define DIAG_STEPN(tag, ...)	((void)0)
#define DIAG_RESET()		((void)0)

#endif /* DIAG */

#endif
