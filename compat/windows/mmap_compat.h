#ifndef NG_WIN_MMAP_COMPAT_H
#define NG_WIN_MMAP_COMPAT_H

/*
 * NG Windows mmap compatibility layer (x64).
 * Substitui mmap/munmap/mprotect/mincore/madvise pelo API de memoria do Windows
 * (VirtualAlloc/VirtualFree/VirtualProtect). O JIT aloca regiao RWX via
 * VirtualAlloc com PAGE_EXECUTE_READWRITE.
 */
#include <windows.h>

#ifndef PROT_NONE
#define PROT_NONE 0
#endif
#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC 0x4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 1
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 2
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

static inline void* ng_win_mmap(size_t len, int prot, int flags) {
    DWORD protect = PAGE_READONLY;
    if (prot & PROT_WRITE) protect = PAGE_READWRITE;
    if (prot & PROT_EXEC) protect = PAGE_EXECUTE_READWRITE;
    if (prot == PROT_READ) protect = PAGE_READONLY;
    void* addr = VirtualAlloc(NULL, (SIZE_T)len, MEM_COMMIT | MEM_RESERVE, protect);
    return addr == NULL ? MAP_FAILED : addr;
}

static inline int ng_win_munmap(void* addr, size_t len) {
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int ng_win_mprotect(void* addr, size_t len, int prot) {
    DWORD protect = PAGE_READONLY;
    if (prot & PROT_WRITE) protect = PAGE_READWRITE;
    if (prot & PROT_EXEC) protect = PAGE_EXECUTE_READWRITE;
    return VirtualProtect(addr, (SIZE_T)len, protect, NULL) ? 0 : -1;
}

static inline int ng_win_madvise(void* addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

static inline int ng_win_mincore(const void* addr, size_t len) {
    (void)addr; (void)len;
    return 0;
}

#endif /* NG_WIN_MMAP_COMPAT_H */
