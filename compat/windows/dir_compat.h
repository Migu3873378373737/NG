#ifndef NG_WIN_DIR_COMPAT_H
#define NG_WIN_DIR_COMPAT_H

#include <direct.h>
#include <sys/stat.h>

/* POSIX mkdir(path, mode) -> Windows _mkdir(path) (o modo e ignorado). */
#define mkdir(path, mode) _mkdir(path)

#endif
