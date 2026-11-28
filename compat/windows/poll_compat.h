#ifndef NG_WIN_POLL_COMPAT_H
#define NG_WIN_POLL_COMPAT_H

#include <winsock2.h>

#define poll(fd, nfds, timeout_ms) WSAPoll((fd), (nfds), (timeout_ms))

#endif /* NG_WIN_POLL_COMPAT_H */
