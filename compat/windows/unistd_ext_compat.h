#ifndef NG_WINDOWS_UNISTD_EXT_COMPAT_H
#define NG_WINDOWS_UNISTD_EXT_COMPAT_H

/*
 * NG Windows unistd extension compatibility layer (x64).
 * Substitui close/read/write/unlink/realpath por APIs do Windows.
 */
#ifndef _WIN32
#define _WIN32
#endif

#include <windows.h>
#include <io.h>
#include <unistd.h>
#include <sys/types.h>

static inline int ng_win_close_fd(int fd) { return _close((int)fd); }
static inline int ng_win_read_fd(int fd, void* buf, int count) { return _read((int)fd, buf, (unsigned int)count); }
static inline int ng_win_write_fd(int fd, const void* buf, int count) { return _write((int)fd, buf, (unsigned int)count); }
static inline int ng_win_unlink(const char* path) { return _unlink(path); }

#define close(fd) ng_win_close_fd(fd)
#define read(fd, buf, count) ng_win_read_fd(fd, buf, count)
#define write(fd, buf, count) ng_win_write_fd(fd, buf, count)
#define unlink(path) ng_win_unlink(path)

#endif /* NG_WINDOWS_UNISTD_EXT_COMPAT_H */
