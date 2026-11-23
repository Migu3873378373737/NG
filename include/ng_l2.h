#ifndef NG_L2_H
#define NG_L2_H

#include "ng_ast.h"
#include "ng_ir.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* L2 (RAM) do Dynamic V: filas circulares, buffers, símbolos, metadados,
   limites de memória e estatísticas do barramento. */

#define NG_L2_TASK_CAP 1024
#define NG_L2_RESULT_CAP 1024
#define NG_L2_SYMBOLS_MAX 512
#define NG_L2_META_KEYS_MAX 128

typedef struct NGL2OptStats {
    uint64_t folds;
    uint64_t dce;
    uint64_t alg;
    uint64_t functions;
    uint64_t globals;
    uint64_t loops;
    uint64_t calls;
    double elapsed_ms;
} NGL2OptStats;

typedef struct NGL2Task {
    NGAstNode* ast;        /* AST a ser otimizada pelo Core 1 */
    const char* filepath;  /* dono do programa (lifetime do chamador) */
    uint64_t seq;          /* sequencial de despacho para correlação */
} NGL2Task;

typedef struct NGL2Result {
    NGAstNode* ast;        /* AST otimizada (ownership segue para Core 0) */
    uint8_t* bytecode;     /* bytecode serializado do IR */
    size_t bytecode_len;
    const char* filepath;  /* dono do resultado */
    uint64_t seq;
    bool success;
    NGL2OptStats stats;
} NGL2Result;

typedef struct NGL2Stats {
    _Atomic uint64_t tasks_pushed;
    _Atomic uint64_t tasks_popped;
    _Atomic uint64_t results_pushed;
    _Atomic uint64_t results_popped;
    _Atomic uint64_t sync_waits;      /* waits em filas vazias */
    _Atomic uint64_t sync_timeouts;
    _Atomic size_t peak_bytes;
    _Atomic size_t current_bytes;
    _Atomic uint64_t symbols_added;
    _Atomic uint64_t metadata_set;
} NGL2Stats;

typedef struct NGL2Queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool shutdown;
    size_t head;   /* índice absoluto */
    size_t tail;
    size_t count;
} NGL2Queue;

typedef struct NGL2Bus {
    NGL2Queue tasks;                                  /* L2 -> Core 1 */
    NGL2Queue results;                                /* Core 1 -> L2/Core 0 */
    NGL2Task task_items[NG_L2_TASK_CAP];
    NGL2Result result_items[NG_L2_RESULT_CAP];
    /* Workspace reutilizável de memória do L2 (malocs/reallocs) */
    uint8_t* workspace;
    size_t workspace_len;
    /* Buffers observáveis do L2 (IR + bytecode gerados pelo Core 1) */
    uint8_t* bytecode;
    size_t bytecode_len;
    NGLIRNode* ir_tree;
    size_t ir_node_count;
    /* Símbolos coletados pelo Core 1 */
    char* symbols[NG_L2_SYMBOLS_MAX];
    size_t symbol_count;
    /* Metadados do programa */
    char* meta_keys[NG_L2_META_KEYS_MAX];
    char* meta_values[NG_L2_META_KEYS_MAX];
    size_t meta_count;
} NGL2Bus;

void ng_l2_init(void);
void ng_l2_shutdown(void);
void ng_l2_shutdown_queues(void);

/* Filas circulares (C11 sync: mutex + cond + atomics) */
bool ng_l2_push_task(const NGL2Task* task);
bool ng_l2_pop_task(NGL2Task* out);
bool ng_l2_pop_task_timeout(NGL2Task* out, int timeout_ms, bool* timed_out);
bool ng_l2_push_result(const NGL2Result* result);
bool ng_l2_pop_result_timeout(NGL2Result* out, int timeout_ms, bool* timed_out);

/* Buffers de memória do L2 */
void ng_l2_track_bytes(long delta);
size_t ng_l2_current_bytes(void);
size_t ng_l2_peak_bytes(void);

/* Buffers observáveis (IR + bytecode) */
void ng_l2_set_bytecode(uint8_t* data, size_t len);
void ng_l2_set_ir(NGLIRNode* ir, size_t node_count);

/* Workspace: bloco scratch reutilizável (busca/gravação de buffers) */
void* ng_l2_workspace_get(size_t size);

/* Limpeza de observáveis (bytecode/IR/símbolos/metadados) */
void ng_l2_clear_observables(void);

/* Símbolos e metadados */
void ng_l2_symbol_add(const char* name);
size_t ng_l2_symbols_count(void);
const char* ng_l2_symbol_at(size_t index);
void ng_l2_meta_set(const char* key, const char* value);
const char* ng_l2_meta_get(const char* key);

/* Estatísticas do barramento */
const NGL2Stats* ng_l2_stats(void);

#endif /* NG_L2_H */
