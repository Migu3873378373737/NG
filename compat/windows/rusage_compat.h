#ifndef NG_WIN_RUSAGE_COMPAT_H
#define NG_WIN_RUSAGE_COMPAT_H

#include <windows.h>
#include <psapi.h>

#define RUSAGE_SELF 0

struct rusage {
    size_t ru_maxrss;
};

static inline int ng_win_getrusage(int who, struct rusage* usage) {
    (void)who;
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        usage->ru_maxrss = (size_t)pmc.PeakWorkingSetSize;
        return 0;
    }
    usage->ru_maxrss = 0;
    return -1;
}

#ifndef getrusage
#define getrusage(who, usage) ng_win_getrusage((who), (usage))
#endif

#endif /* NG_WIN_RUSAGE_COMPAT_H */
