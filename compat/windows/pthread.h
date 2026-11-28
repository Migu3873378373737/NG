#ifndef NG_WIN_PTHREAD_H
#define NG_WIN_PTHREAD_H

/*
 * NG Windows pthread compatibility layer (x64).
 * Porta os mecanismos de thread/mutex/condicao do Dynamic V (L2 Barramento,
 * Core 1 worker) para a API nativa do Windows.
 *
 *   - pthread_create / pthread_join  -> CreateThread / WaitForSingleObject.
 *   - pthread_mutex_*                -> Semaphore kernel (contagem binaria).
 *   - pthread_cond_* (signal/broadcast/wait/timedwait) -> Event (manual reset).
 */

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>

typedef HANDLE pthread_t;

typedef struct NGWinMutex {
    HANDLE sem;
} pthread_mutex_t;

typedef struct NGWinCond {
    HANDLE ev;
} pthread_cond_t;

static inline int ng_win_pthread_mutex_init(pthread_mutex_t* mutex, const void* attr) {
    (void)attr;
    if (!mutex) return -1;
    mutex->sem = CreateSemaphore(NULL, 1, 1, NULL);
    return mutex->sem ? 0 : -1;
}

static inline int ng_win_pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    if (mutex->sem) { CloseHandle(mutex->sem); mutex->sem = NULL; }
    return 0;
}

/* Semaphore kernel como mutex binario (inicializada com count=1). */
static inline int ng_win_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!mutex || !mutex->sem) return -1;
    return WaitForSingleObject(mutex->sem, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

static inline int ng_win_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex || !mutex->sem) return -1;
    return ReleaseSemaphore(mutex->sem, 1, NULL) == 1 ? 0 : -1;
}

static inline int ng_win_pthread_cond_init(pthread_cond_t* cond, const void* attr) {
    (void)attr;
    if (!cond) return -1;
    cond->ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    return cond->ev ? 0 : -1;
}

static inline int ng_win_pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cond) return -1;
    if (cond->ev) { CloseHandle(cond->ev); cond->ev = NULL; }
    return 0;
}

static inline int ng_win_pthread_cond_signal(pthread_cond_t* cond) {
    if (!cond || !cond->ev) return -1;
    return SetEvent(cond->ev) ? 0 : -1;
}

static inline int ng_win_pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cond || !cond->ev) return -1;
    return SetEvent(cond->ev) ? 0 : -1;
}

static inline int ng_win_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cond || !mutex) return -1;
    ng_win_pthread_mutex_unlock(mutex);
    DWORD rc = WaitForSingleObject(cond->ev, INFINITE);
    ng_win_pthread_mutex_lock(mutex);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}

static inline int ng_win_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* ts) {
    if (!cond || !mutex) return -1;
    ng_win_pthread_mutex_unlock(mutex);
    DWORD ms = INFINITE;
    if (ts) {
        long long sec = (long long)ts->tv_sec;
        long long nsec = (long long)ts->tv_nsec;
        long long ms_total = sec * 1000LL + (nsec / 1000000LL);
        if (ms_total > 0 && ms_total < 86400000LL) ms = (DWORD)ms_total;
    }
    DWORD rc = WaitForSingleObject(cond->ev, ms);
    ng_win_pthread_mutex_lock(mutex);
    return (rc == WAIT_OBJECT_0 || rc == WAIT_TIMEOUT) ? 0 : -1;
}

/* Implementacao nativa Win32 dos metodos do modulo pthread (portabilidade). */
static inline int pthread_mutex_init(pthread_mutex_t* mutex, const void* attr) {
    return ng_win_pthread_mutex_init(mutex, attr);
}

static inline int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    return ng_win_pthread_mutex_destroy(mutex);
}

static inline int pthread_mutex_lock(pthread_mutex_t* mutex) {
    return ng_win_pthread_mutex_lock(mutex);
}

static inline int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    return ng_win_pthread_mutex_unlock(mutex);
}

static inline int pthread_cond_init(pthread_cond_t* cond, const void* attr) {
    return ng_win_pthread_cond_init(cond, attr);
}

static inline int pthread_cond_destroy(pthread_cond_t* cond) {
    return ng_win_pthread_cond_destroy(cond);
}

static inline int pthread_cond_signal(pthread_cond_t* cond) {
    return ng_win_pthread_cond_signal(cond);
}

static inline int pthread_cond_broadcast(pthread_cond_t* cond) {
    return ng_win_pthread_cond_broadcast(cond);
}

static inline int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    return ng_win_pthread_cond_wait(cond, mutex);
}

static inline int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* ts) {
    return ng_win_pthread_cond_timedwait(cond, mutex, ts);
}

static inline int ng_win_pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg) {
    if (!thread || !start) return -1;
    pthread_t h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)start, arg, 0, NULL);
    if (!h) return -1;
    *thread = h;
    return 0;
}

static inline int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg) {
    return ng_win_pthread_create(thread, attr, start, arg);
}

static inline int pthread_join(pthread_t thread, void** res) {
    if (!thread) return -1;
    DWORD rc = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}

#endif /* _WIN32 */

#endif /* NG_WIN_PTHREAD_H */
