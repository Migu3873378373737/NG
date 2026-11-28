#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_l2.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
/* clock_gettime -> QueryPerformanceCounter: o MinGW declara clock_gettime()
   como inline que chama clock_gettime64() da libwinpthread, ausente no
   runtime Windows da NG. */
#include "../compat/windows/posix_compat.h"
#endif

static NGL2Bus g_l2;
static bool g_l2_initialized = false;
static _Atomic size_t g_current_bytes = 0;
static _Atomic size_t g_peak_bytes = 0;
static NGL2Stats g_stats;

static double l2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static size_t l2_remaining_ms(double t0, int timeout_ms) {
    double now = l2_now_ms();
    double left = (double)timeout_ms - (now - t0);
    if (left < 0.0) return 0;
    if (left > (double)timeout_ms) return (size_t)timeout_ms;
    return (size_t)left;
}

static void track_peak(size_t cur) {
    size_t peak = atomic_load(&g_peak_bytes);
    while (cur > peak) {
        if (atomic_compare_exchange_weak(&g_peak_bytes, &peak, cur)) break;
    }
}

void ng_l2_track_bytes(long delta) {
    if (delta > 0) {
        long prev = atomic_fetch_add((_Atomic long*)&g_current_bytes, delta);
        (void)prev;
        track_peak((size_t)atomic_load(&g_current_bytes));
    } else if (delta < 0) {
        atomic_fetch_add((_Atomic long*)&g_current_bytes, delta);
    }
}

size_t ng_l2_current_bytes(void) {
    return atomic_load(&g_current_bytes);
}

size_t ng_l2_peak_bytes(void) {
    return atomic_load(&g_peak_bytes);
}

static void queue_init(NGL2Queue* q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->shutdown = false;
    q->head = q->tail = q->count = 0;
}

static void queue_destroy(NGL2Queue* q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
}

void ng_l2_init(void) {
    if (g_l2_initialized) return;
    memset(&g_l2, 0, sizeof(g_l2));
    g_l2_initialized = true;
    queue_init(&g_l2.tasks);
    queue_init(&g_l2.results);
}

void ng_l2_shutdown_queues(void) {
    if (!g_l2_initialized) return;
    g_l2.tasks.shutdown = true;
    pthread_cond_broadcast(&g_l2.tasks.cv);
    g_l2.results.shutdown = true;
    pthread_cond_broadcast(&g_l2.results.cv);
}

void ng_l2_shutdown(void) {
    if (!g_l2_initialized) return;
    queue_destroy(&g_l2.tasks);
    queue_destroy(&g_l2.results);

    free(g_l2.bytecode);
    g_l2.bytecode = NULL;
    g_l2.bytecode_len = 0;
    ng_l2_set_ir(NULL, 0);
    for (size_t i = 0; i < g_l2.symbol_count; i++) free(g_l2.symbols[i]);
    g_l2.symbol_count = 0;
    for (size_t i = 0; i < g_l2.meta_count; i++) {
        free(g_l2.meta_keys[i]);
        free(g_l2.meta_values[i]);
    }
    g_l2.meta_count = 0;
    g_l2_initialized = false;
}

/* ---------------- Fila circular (C11: mutex + cond + atomics) ---------------- */

static bool queue_push_task(const NGL2Task* t) {
    NGL2Queue* q = &g_l2.tasks;
    pthread_mutex_lock(&q->mu);
    if (q->count >= NG_L2_TASK_CAP) {
        pthread_mutex_unlock(&q->mu);
        return false;
    }
    g_l2.task_items[q->tail] = *t;
    q->tail = (q->tail + 1) % NG_L2_TASK_CAP;
    q->count++;
    atomic_fetch_add(&g_stats.tasks_pushed, 1);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static bool queue_pop_task_timeout(NGL2Task* out, int timeout_ms, bool* timed_out) {
    NGL2Queue* q = &g_l2.tasks;
    double t0 = l2_now_ms();
    bool got = false;
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->shutdown) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->cv, &q->mu);
            atomic_fetch_add(&g_stats.sync_waits, 1);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000);
            ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int rc = pthread_cond_timedwait(&q->cv, &q->mu, &ts);
            atomic_fetch_add(&g_stats.sync_waits, 1);
            if (rc == ETIMEDOUT && q->count == 0) {
                atomic_fetch_add(&g_stats.sync_timeouts, 1);
                break;
            }
        }
        if (l2_remaining_ms(t0, timeout_ms) == 0 && q->count == 0) break;
    }
    if (q->count > 0) {
        *out = g_l2.task_items[q->head];
        q->head = (q->head + 1) % NG_L2_TASK_CAP;
        q->count--;
        atomic_fetch_add(&g_stats.tasks_popped, 1);
        got = true;
    }
    pthread_mutex_unlock(&q->mu);
    if (timed_out) *timed_out = !got;
    return got;
}

static bool queue_push_result(const NGL2Result* r) {
    NGL2Queue* q = &g_l2.results;
    pthread_mutex_lock(&q->mu);
    if (q->count >= NG_L2_RESULT_CAP) {
        pthread_mutex_unlock(&q->mu);
        return false;
    }
    g_l2.result_items[q->tail] = *r;
    q->tail = (q->tail + 1) % NG_L2_RESULT_CAP;
    q->count++;
    atomic_fetch_add(&g_stats.results_pushed, 1);
    ng_l2_track_bytes((long)r->bytecode_len);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static bool queue_pop_result_timeout(NGL2Result* out, int timeout_ms, bool* timed_out) {
    NGL2Queue* q = &g_l2.results;
    bool got = false;
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->shutdown) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->cv, &q->mu);
            atomic_fetch_add(&g_stats.sync_waits, 1);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int rc = pthread_cond_timedwait(&q->cv, &q->mu, &ts);
            atomic_fetch_add(&g_stats.sync_waits, 1);
            if (rc == ETIMEDOUT && q->count == 0) break;
        }
    }
    if (q->count > 0) {
        *out = g_l2.result_items[q->head];
        ng_l2_track_bytes(-(long)out->bytecode_len);
        q->head = (q->head + 1) % NG_L2_RESULT_CAP;
        q->count--;
        atomic_fetch_add(&g_stats.results_popped, 1);
        got = true;
    }
    pthread_mutex_unlock(&q->mu);
    if (timed_out) *timed_out = !got;
    return got;
}

bool ng_l2_push_task(const NGL2Task* task) { return queue_push_task(task); }

bool ng_l2_pop_task(NGL2Task* out) {
    bool to;
    return queue_pop_task_timeout(out, -1, &to);
}

bool ng_l2_pop_task_timeout(NGL2Task* out, int timeout_ms, bool* timed_out) {
    return queue_pop_task_timeout(out, timeout_ms, timed_out);
}

bool ng_l2_push_result(const NGL2Result* result) { return queue_push_result(result); }

bool ng_l2_pop_result_timeout(NGL2Result* out, int timeout_ms, bool* timed_out) {
    return queue_pop_result_timeout(out, timeout_ms, timed_out);
}

/* ---------------- Buffers observáveis do L2 ---------------- */

void ng_l2_set_bytecode(uint8_t* data, size_t len) {
    ng_l2_track_bytes((long)len - (long)g_l2.bytecode_len);
    if (g_l2.bytecode) free(g_l2.bytecode);
    g_l2.bytecode = data;
    g_l2.bytecode_len = len;
}

void ng_l2_set_ir(NGLIRNode* ir, size_t node_count) {
    if (g_l2.ir_tree) ng_ir_free(g_l2.ir_tree);
    g_l2.ir_tree = ir;
    g_l2.ir_node_count = node_count;
}

/* ---------------- Símbolos e metadados ---------------- */

void ng_l2_symbol_add(const char* name) {
    if (!name || g_l2.symbol_count >= NG_L2_SYMBOLS_MAX) return;
    for (size_t i = 0; i < g_l2.symbol_count; i++) {
        if (g_l2.symbols[i] && strcmp(g_l2.symbols[i], name) == 0) return;
    }
    g_l2.symbols[g_l2.symbol_count] = strdup(name);
    if (g_l2.symbols[g_l2.symbol_count]) g_l2.symbol_count++;
    atomic_fetch_add(&g_stats.symbols_added, 1);
}

size_t ng_l2_symbols_count(void) { return g_l2.symbol_count; }

const char* ng_l2_symbol_at(size_t index) {
    if (index >= g_l2.symbol_count) return NULL;
    return g_l2.symbols[index];
}

void ng_l2_meta_set(const char* key, const char* value) {
    if (!key || !value || g_l2.meta_count >= NG_L2_META_KEYS_MAX) return;
    for (size_t i = 0; i < g_l2.meta_count; i++) {
        if (strcmp(g_l2.meta_keys[i], key) == 0) {
            free(g_l2.meta_values[i]);
            g_l2.meta_values[i] = strdup(value);
            return;
        }
    }
    g_l2.meta_keys[g_l2.meta_count] = strdup(key);
    g_l2.meta_values[g_l2.meta_count] = strdup(value);
    if (g_l2.meta_keys[g_l2.meta_count]) {
        g_l2.meta_count++;
        atomic_fetch_add(&g_stats.metadata_set, 1);
    }
}

const char* ng_l2_meta_get(const char* key) {
    if (!key) return NULL;
    for (size_t i = 0; i < g_l2.meta_count; i++) {
        if (strcmp(g_l2.meta_keys[i], key) == 0) return g_l2.meta_values[i];
    }
    return NULL;
}

void* ng_l2_workspace_get(size_t size) {
    if (size > g_l2.workspace_len) {
        uint8_t* p = (uint8_t*)realloc(g_l2.workspace, size);
        if (!p) return NULL;
        g_l2.workspace = p;
        g_l2.workspace_len = size;
        ng_l2_track_bytes((long)size);
    }
    return g_l2.workspace;
}

void ng_l2_clear_observables(void) {
    ng_l2_set_bytecode(NULL, 0);
    ng_l2_set_ir(NULL, 0);
    for (size_t i = 0; i < g_l2.symbol_count; i++) free(g_l2.symbols[i]);
    g_l2.symbol_count = 0;
    for (size_t i = 0; i < g_l2.meta_count; i++) {
        free(g_l2.meta_keys[i]);
        free(g_l2.meta_values[i]);
    }
    g_l2.meta_count = 0;
}

const NGL2Stats* ng_l2_stats(void) { return &g_stats; }
