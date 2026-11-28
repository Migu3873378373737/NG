#ifndef NG_WIN_STRING_COMPAT_H
#define NG_WIN_STRING_COMPAT_H

#include <string.h>
#include <ctype.h>

/* strcasestr: case-insensitive strstr (POSIX). */
static inline char* ng_win_strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t len = strlen(needle);
    if (len == 0) return (char*)haystack;
    for (const char* h = haystack; *h; h++) {
        if (toupper((unsigned char)*h) == toupper((unsigned char)needle[0])) {
            size_t i = 0;
            while (h[i] && needle[i] && toupper((unsigned char)h[i]) == toupper((unsigned char)needle[i])) i++;
            if (needle[i] == '\0') return (char*)haystack;
        }
    }
    return NULL;
}

#ifndef strcasestr
#define strcasestr(h, n) ng_win_strcasestr(h, n)
#endif

#endif /* NG_WIN_STRING_COMPAT_H */
