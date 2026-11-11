/* harness shim: the host's libstdc++ <atomic> expects Linux's futex syscall.
 * Haiku's libstdc++ is built without it. Declarations only; nothing here is
 * ever linked. */
#ifndef CROSSCHECK_SYSCALL_H
#define CROSSCHECK_SYSCALL_H
#define SYS_futex 202
#ifdef __cplusplus
extern "C"
#endif
long syscall(long number, ...);
#endif
