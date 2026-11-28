#ifndef NG_WINDOWS_UNISTD_COMPAT_H
#define NG_WINDOWS_UNISTD_COMPAT_H

/*
 * NG Windows unistd compatibility layer (x64).
 * Wrappers POSIX -> Windows para APIs de processo/arquivo basicas.
 */

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <unistd.h>
#include <sys/types.h>

/*
 * clock_gettime/nanosleep: o MinGW declara clock_gettime()/nanosleep() em
 * <pthread_time.h> como inline que chamam clock_gettime64()/nanosleep64() da
 * libwinpthread, que nao existe no runtime Windows da NG. O posix_compat.h
 * fornece implementacoes Win32 (QueryPerformanceCounter/Sleep) e macros que
 * substituem as chamadas antes de qualquer uso.
 */
#include "posix_compat.h"

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 1
#endif

static inline int ng_win_close(int fd) { return _close((int)fd); }
static inline int ng_win_read(int fd, void* buf, int count) { return _read((int)fd, buf, (unsigned int)count); }
static inline int ng_win_write(int fd, const void* buf, int count) { return _write((int)fd, buf, (unsigned int)count); }
static inline int ng_win_open(const char* path, int oflag, int perm) { return _open(path, oflag, perm); }

static inline long ng_win_sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return -1;
}

#define sysconf(name) ng_win_sysconf(name)

#define close(fd) ng_win_close(fd)
#define read(fd, buf, count) ng_win_read(fd, buf, count)
#define write(fd, buf, count) ng_win_write(fd, buf, count)
#define open(path, oflag, perm) ng_win_open(path, oflag, perm)

#else

#include <unistd.h>
#include <sys/types.h>

#endif /* _WIN32 */

#endif /* NG_WINDOWS_UNISTD_COMPAT_H */
