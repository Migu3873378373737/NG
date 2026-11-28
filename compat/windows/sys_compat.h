#ifndef NG_WIN_SYS_COMPAT_H
#define NG_WIN_SYS_COMPAT_H

/*
 * NG Windows POSIX system constants compatibility layer (x64).
 * Constantes POSIX indisponiveis no MinGW (<time.h>, <errno.h>, <signal.h>).
 */

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

/* WIFSIGNALED/WEXITSTATUS/... do processo (usado pelo modulo @process). */
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#define WTERMSIG(status) ((status) & 0x7f)
#define WSTOPSIG(status) ((status) & 0xff00)

/* Error codes POSIX usados pelo barramento L2/DynamicV */
#ifndef ETIMEDOUT
#define ETIMEDOUT WAIT_TIMEOUT
#endif

#endif /* NG_WIN_SYS_COMPAT_H */
