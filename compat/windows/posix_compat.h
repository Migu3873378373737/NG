#ifndef NG_WINDOWS_POSIX_COMPAT_H
#define NG_WINDOWS_POSIX_COMPAT_H

/*
 * NG Windows POSIX Compatibility Layer (x64)
 * ==========================================
 * Camada organizada de compatibilidade entre a API POSIX usada pela
 * linguagem NG (Linux/Android/Termux) e a API nativa do Windows.
 *
 *   - Variaveis de ambiente  -> Get/Set/Unset via API do Windows.
 *   - Sistema de arquivos     -> mkdir/rmdir/stat/access/unlink/rename/getcwd.
 *   - Tempo                    -> clock_gettime/nanosleep/usleep/sleep/time.
 *   - Diagnostico             -> uname/system info, CPUs, memoria.
 *
 * Em plataformas POSIX (Linux/Android) este cabecalho nao faz nada: os
 * sources incluem as headers POSIX nativas diretamente.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef EINTR
#define EINTR WSAEINTR
#endif
#ifndef EEXIST
#define EEXIST ERROR_ALREADY_EXISTS
#endif
#ifndef ENOENT
#define ENOENT ERROR_FILE_NOT_FOUND
#endif
#ifndef EACCES
#define EACCES ERROR_ACCESS_DENIED
#endif
#ifndef ENOMEM
#define ENOMEM ERROR_NOT_ENOUGH_MEMORY
#endif
#ifndef EINVAL
#define EINVAL ERROR_INVALID_PARAMETER
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 1
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

/*
 * Variaveis de ambiente (POSIX getenv/setenv/unsetenv -> Windows API).
 */
static inline char* ng_win_getenv(const char* name) {
    static char buf[32768];
    if (!name) { buf[0] = '\0'; return NULL; }
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    if (n == 0) { buf[0] = '\0'; return NULL; }
    buf[n] = '\0';
    return buf;
}

static inline int ng_win_setenv(const char* name, const char* value, int overwrite) {
    if (!name) return -1;
    if (!overwrite && ng_win_getenv(name)[0] != '\0') return 0;
    if (!value) {
        SetEnvironmentVariableA(name, "");
        return 0;
    }
    if (value[0] == '\0') {
        SetEnvironmentVariableA(name, "");
        return 0;
    }
    SetEnvironmentVariableA(name, value);
    return 0;
}

static inline int ng_win_unsetenv(const char* name) {
    if (!name) return -1;
    SetEnvironmentVariableA(name, "");
    return 0;
}

#define getenv(name) ng_win_getenv(name)
#define setenv(name, value, overwrite) ng_win_setenv(name, value, overwrite)
#define unsetenv(name) ng_win_unsetenv(name)

/*
 * Sistema de arquivos (POSIX -> _* Win32 APIs).
 */
/* Nota: mkdir/rmdir/unlink/access/rename/getcwd usam as _* Win32 equivalents
   declaradas nos headers do MinGW, portanto nao ha mapeamento de macros. */

/*
 * Tempo (POSIX -> APIs Win32).
 */
static inline int ng_win_clock_gettime(clockid_t clock_id, struct timespec* ts) {
    if (clock_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq;
        LARGE_INTEGER cnt;
        if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&cnt)) {
            unsigned long long f = freq.QuadPart;
            unsigned long long c = cnt.QuadPart;
            ts->tv_sec = (time_t)(c / f);
            ts->tv_nsec = (long)(1000000000LL * (c % f) / f);
            return 0;
        }
    }
    if (clock_id == CLOCK_REALTIME) {
        ts->tv_sec = time(NULL);
        ts->tv_nsec = 0;
        return 0;
    }
    return -1;
}

#define clock_gettime(clock_id, tp) ng_win_clock_gettime((clock_id), (tp))

static inline int ng_win_nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)rem;
    if (req) {
        if (req->tv_sec > 0) Sleep((DWORD)req->tv_sec * 1000);
        else if (req->tv_nsec > 0) Sleep((DWORD)((req->tv_nsec + 999999999) / 1000000));
    }
    return 0;
}

#ifndef nanosleep
#define nanosleep(req, rem) ng_win_nanosleep((req), (rem))
#endif

static inline int ng_win_usleep(useconds_t us) {
    if (us > 0) Sleep((DWORD)us / 1000);
    return 0;
}

#ifndef usleep
#define usleep(us) ng_win_usleep(us)
#endif

static inline int ng_win_sleep(unsigned int secs) {
    Sleep((DWORD)secs);
    return 0;
}

#ifndef sleep
#define sleep(s) ng_win_sleep(s)
#endif

/*
 * Structs POSIX indisponiveis no MinGW (x64).
 */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

struct sysinfo {
    long totalram;
    long freeram;
    long usermem;
    long uptime;
    long mem_unit;
};

static inline int ng_win_uname(struct utsname* u) {
    if (!u) return -1;
    char comp[256];
    DWORD len = (DWORD)sizeof(comp);
    if (GetComputerNameA(comp, &len)) {
        snprintf(u->nodename, sizeof(u->nodename), "%s", comp);
    } else {
        snprintf(u->nodename, sizeof(u->nodename), "unknown");
    }
    snprintf(u->sysname, sizeof(u->sysname), "Windows");
    snprintf(u->release, sizeof(u->release), "%lu", (unsigned long)GetVersion());
    snprintf(u->version, sizeof(u->version), "Windows NT");
    snprintf(u->machine, sizeof(u->machine), "x86_64");
    return 0;
}

static inline int ng_win_sysinfo(struct sysinfo* si) {
    if (!si) return -1;
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        si->totalram = (long)(ms.ullTotalPhys / (1024 * 1024));
        si->freeram = (long)(ms.ullAvailPhys / (1024 * 1024));
        si->usermem = (long)(ms.ullTotalPhys - ms.ullAvailPhys);
        si->mem_unit = 1;
        return 0;
    }
    return -1;
}

#ifndef uname
static inline int uname(struct utsname* u) { return ng_win_uname(u); }
#endif
#ifndef sysinfo
static inline int sysinfo(struct sysinfo* si) { return ng_win_sysinfo(si); }
#endif

#endif /* _WIN32 */

#endif /* NG_WINDOWS_POSIX_COMPAT_H */
