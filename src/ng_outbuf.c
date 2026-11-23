#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_outbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NG_OUTBUF_MAX (4u * 1024u * 1024u)

static char* g_buf = NULL;
static size_t g_len = 0;
static size_t g_cap = 0;
static bool g_enabled = true;
static bool g_inited = false;

static void outbuf_init(void) {
    if (g_inited) return;
    g_inited = true;
    const char* dis = getenv("NG_OUTBUF_DISABLE");
    if (dis && (strcmp(dis, "1") == 0 || strcmp(dis, "true") == 0)) g_enabled = false;
    if (g_enabled) {
        g_cap = 4096;
        g_buf = (char*)malloc(g_cap);
        if (g_buf) g_buf[0] = '\0'; else g_cap = 0;
    }
}

bool ng_outbuf_enabled(void) {
    outbuf_init();
    return g_enabled;
}

size_t ng_outbuf_len(void) { return g_len; }

void ng_outbuf_write(const char* data, size_t len) {
    outbuf_init();
    if (!data || len == 0) return;
    if (!g_enabled) {
        fwrite(data, 1, len, stdout);
        return;
    }
    if (g_len + len + 1 > NG_OUTBUF_MAX) {
        /* Overflow protegido: escreve o acumulado e o fragmento direto. */
        ng_outbuf_flush();
        fwrite(data, 1, len, stdout);
        return;
    }
    if (g_len + len + 1 > g_cap) {
        size_t ncap = g_cap ? g_cap : 4096;
        while (ncap < g_len + len + 1) ncap *= 2;
        char* p = (char*)realloc(g_buf, ncap);
        if (!p) {
            ng_outbuf_flush();
            fwrite(data, 1, len, stdout);
            return;
        }
        g_buf = p;
        g_cap = ncap;
    }
    memcpy(g_buf + g_len, data, len);
    g_len += len;
    g_buf[g_len] = '\0';
}

void ng_outbuf_printf(const char* fmt, ...) {
    if (!ng_outbuf_enabled()) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        return;
    }
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(tmp)) {
        char* big = (char*)malloc((size_t)n + 1);
        if (!big) return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        ng_outbuf_write(big, (size_t)n);
        free(big);
        return;
    }
    ng_outbuf_write(tmp, (size_t)n);
}

void ng_outbuf_flush(void) {
    outbuf_init();
    if (g_buf && g_len > 0) {
        fwrite(g_buf, 1, g_len, stdout);
        fflush(stdout);
    }
    free(g_buf);
    g_buf = NULL;
    g_len = 0;
    g_cap = 0;
}
