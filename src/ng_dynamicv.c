#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_dynamicv.h"
#include "ng_lexer.h"
#include "ng_parser.h"
#include "ng_n3cache.h"
#include "ng_l2.h"
#include "ng_ir.h"
#include "ng_lua_optimizer.h"
#include "ng_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include "../compat/windows/pthread.h"
#include "../compat/windows/rusage_compat.h"
#include "../compat/windows/unistd_compat.h"
#else
#include <unistd.h>
#include <sys/resource.h>
#include <pthread.h>
#endif

#ifdef NG_HAS_EMBEDDED_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#endif

/* ------------------------------------------------------------
 * Estado do Dynamic V (Core 0 + Core 1 + L2 + Barramento)
 * ------------------------------------------------------------ */

static struct {
    bool initialized;
    bool enabled;
    NGDynamicVStats stats;
    NGDynamicVState core0_state;
    NGDynamicVState core1_state;
    pthread_t worker_thread;
    _Atomic bool worker_running;
    _Atomic bool shutdown_requested;
    uint64_t next_seq;
#ifdef NG_HAS_EMBEDDED_LUA
    lua_State* L;
#endif
} g_dv;

#define NG_DV_RESULT_WAIT_MS 500
#define NG_DV_POP_MS 10

/* ------------------------------------------------------------
 * Otimizações semânticas em C (Fallback seguro e base do Core 1)
 * ------------------------------------------------------------ */
static NGAstNode* optimize_node(NGAstNode* node);

static bool is_constant_number(const NGAstNode* node, double* out_num) {
    if (node && node->type == NG_AST_EXPR_LITERAL && node->as.literal_expr.value.type == NG_VAL_NUMBER) {
        if (out_num) *out_num = node->as.literal_expr.value.as.number;
        return true;
    }
    return false;
}

static bool is_constant_string(const NGAstNode* node, const char** out_s) {
    if (node && node->type == NG_AST_EXPR_LITERAL && node->as.literal_expr.value.type == NG_VAL_STRING) {
        if (out_s) *out_s = node->as.literal_expr.value.as.string;
        return true;
    }
    return false;
}

static NGAstNode* optimize_binary_expr(NGAstNode* node) {
    if (!node || node->type != NG_AST_EXPR_BINARY) return node;

    node->as.binary_expr.left = optimize_node(node->as.binary_expr.left);
    node->as.binary_expr.right = optimize_node(node->as.binary_expr.right);

    NGAstNode* left = node->as.binary_expr.left;
    NGAstNode* right = node->as.binary_expr.right;
    int op = node->as.binary_expr.op;

    /* Dobra de strings: "a" + "b" -> "ab" */
    const char* str_l = NULL;
    const char* str_r = NULL;
    if (op == NG_TOK_PLUS && is_constant_string(left, &str_l) && is_constant_string(right, &str_r)) {
        size_t len_l = strlen(str_l), len_r = strlen(str_r);
        char* concat = (char*)malloc(len_l + len_r + 1);
        if (concat) {
            memcpy(concat, str_l, len_l);
            memcpy(concat + len_l, str_r, len_r);
            concat[len_l + len_r] = '\0';
            int line = node->line, col = node->column;
            ng_ast_free(node);
            NGAstNode* folded = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
            folded->as.literal_expr.value = ng_val_string_take(concat);
            atomic_fetch_add(&g_dv.stats.constant_folds, 1);
            atomic_fetch_add(&g_dv.stats.optimizations_applied, 1);
            return folded;
        }
    }

    double num_l = 0, num_r = 0;
    if (is_constant_number(left, &num_l) && is_constant_number(right, &num_r)) {
        double res = 0.0;
        bool is_bool = false;
        bool b_res = false;
        bool can_fold = true;
        switch (op) {
            case NG_TOK_PLUS:    res = num_l + num_r; break;
            case NG_TOK_MINUS:   res = num_l - num_r; break;
            case NG_TOK_STAR:    res = num_l * num_r; break;
            case NG_TOK_SLASH:
                if (num_r != 0.0) res = num_l / num_r; else can_fold = false;
                break;
            case NG_TOK_PERCENT:
                if (num_r != 0.0) res = fmod(num_l, num_r); else can_fold = false;
                break;
            case NG_TOK_EQ:  is_bool = true; b_res = (num_l == num_r); break;
            case NG_TOK_NEQ: is_bool = true; b_res = (num_l != num_r); break;
            case NG_TOK_LT:  is_bool = true; b_res = (num_l < num_r); break;
            case NG_TOK_LTE: is_bool = true; b_res = (num_l <= num_r); break;
            case NG_TOK_GT:  is_bool = true; b_res = (num_l > num_r); break;
            case NG_TOK_GTE: is_bool = true; b_res = (num_l >= num_r); break;
            default: can_fold = false; break;
        }
        if (can_fold) {
            int line = node->line, col = node->column;
            ng_ast_free(node);
            NGAstNode* folded = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
            folded->as.literal_expr.value = is_bool ? ng_val_bool(b_res) : ng_val_number(res);
            atomic_fetch_add(&g_dv.stats.constant_folds, 1);
            atomic_fetch_add(&g_dv.stats.optimizations_applied, 1);
            return folded;
        }
    }
    return node;
}

static NGAstNode* optimize_if_stmt(NGAstNode* node) {
    if (!node || node->type != NG_AST_STMT_IF) return node;
    node->as.if_stmt.condition = optimize_node(node->as.if_stmt.condition);
    node->as.if_stmt.then_block = optimize_node(node->as.if_stmt.then_block);
    if (node->as.if_stmt.else_block) node->as.if_stmt.else_block = optimize_node(node->as.if_stmt.else_block);
    return node;
}

static NGAstNode* optimize_node(NGAstNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case NG_AST_PROGRAM:
            for (size_t i = 0; i < node->as.program.count; i++) {
                NGAstNode* opt = optimize_node(node->as.program.statements[i]);
                if (opt) node->as.program.statements[i] = opt;
            }
            return node;
        case NG_AST_STMT_PRINT:
            node->as.print_stmt.expr = optimize_node(node->as.print_stmt.expr);
            return node;
        case NG_AST_STMT_G:
            if (node->as.g_stmt.expr) node->as.g_stmt.expr = optimize_node(node->as.g_stmt.expr);
            return node;
        case NG_AST_STMT_Y:
            node->as.y_stmt.expr = optimize_node(node->as.y_stmt.expr);
            return node;
        case NG_AST_STMT_ASSIGN:
            node->as.assign_stmt.value = optimize_node(node->as.assign_stmt.value);
            return node;
        case NG_AST_STMT_IF:
            return optimize_if_stmt(node);
        case NG_AST_STMT_WHILE:
            node->as.while_stmt.condition = optimize_node(node->as.while_stmt.condition);
            node->as.while_stmt.block = optimize_node(node->as.while_stmt.block);
            return node;
        case NG_AST_STMT_FUNC:
            node->as.func_stmt.body = optimize_node(node->as.func_stmt.body);
            return node;
        case NG_AST_EXPR_FUNC:
            node->as.func_expr.body = optimize_node(node->as.func_expr.body);
            return node;
        case NG_AST_STMT_LOOP:
            node->as.loop_stmt.iterable = optimize_node(node->as.loop_stmt.iterable);
            node->as.loop_stmt.body = optimize_node(node->as.loop_stmt.body);
            return node;
        case NG_AST_EXPR_BINARY:
            return optimize_binary_expr(node);
        case NG_AST_EXPR_CALL:
            node->as.call_expr.callee = optimize_node(node->as.call_expr.callee);
            for (size_t i = 0; i < node->as.call_expr.arg_count; i++)
                node->as.call_expr.args[i] = optimize_node(node->as.call_expr.args[i]);
            return node;
        case NG_AST_EXPR_TABLE_GET:
            node->as.table_get.table = optimize_node(node->as.table_get.table);
            return node;
        case NG_AST_STMT_TABLE_SET:
            node->as.table_set.table = optimize_node(node->as.table_set.table);
            node->as.table_set.value = optimize_node(node->as.table_set.value);
            return node;
        case NG_AST_EXPR_ARRAY:
            for (size_t i = 0; i < node->as.array_expr.count; i++)
                node->as.array_expr.elements[i] = optimize_node(node->as.array_expr.elements[i]);
            return node;
        case NG_AST_EXPR_INDEX:
            node->as.index_expr.target = optimize_node(node->as.index_expr.target);
            node->as.index_expr.index = optimize_node(node->as.index_expr.index);
            return node;
        default:
            return node;
    }
}

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static const char* state_str(NGDynamicVState s) {
    switch (s) {
        case NG_DV_STATE_UNINITIALIZED: return "Nao Inicializado";
        case NG_DV_STATE_IDLE:          return "Ocioso / Pronto";
        case NG_DV_STATE_OPTIMIZING:    return "Otimizando AST/IR";
        case NG_DV_STATE_EXECUTING:     return "Executando";
        case NG_DV_STATE_FALLBACK:      return "Modo Fallback Core 0";
        case NG_DV_STATE_STOPPED:       return "Parado";
        default:                        return "Desconhecido";
    }
}

/* Coleta símbolos da árvore IR (fila de observáveis do L2) */
static void collect_ir_symbols(NGLIRNode* n) {
    if (!n) return;
    switch (n->type) {
        case NGLIR_STMT_ASSIGN:
        case NGLIR_STMT_FUNC:
        case NGLIR_EXPR_FUNC:
        case NGLIR_STMT_LOOP:
        case NGLIR_STMT_IMPORT:
        case NGLIR_EXPR_IDENT:
            if (n->name) ng_l2_symbol_add(n->name);
            break;
        default: break;
    }
    for (size_t i = 0; i < n->kid_count; i++) collect_ir_symbols(n->kids[i]);
    collect_ir_symbols(n->a);
    collect_ir_symbols(n->b);
}

#ifdef NG_HAS_EMBEDDED_LUA
static void init_embedded_lua(void) {
    g_dv.L = luaL_newstate();
    if (!g_dv.L) return;
    luaL_openlibs(g_dv.L);
    luaL_dostring(g_dv.L, ng_lua_optimizer_script());
}
#endif

/* ---------------- Core 1: Worker Lua embedded ---------------- */

static void* core1_worker_loop(void* arg) {
    (void)arg;
    g_dv.core1_state = NG_DV_STATE_IDLE;
    while (!atomic_load(&g_dv.shutdown_requested)) {
        NGL2Task task;
        if (!ng_l2_pop_task(&task)) break;
        if (!task.ast) break; /* sentinela de desligamento */

        g_dv.core1_state = NG_DV_STATE_OPTIMIZING;
        double t0 = get_time_ms();

        NGL2Result res;
        memset(&res, 0, sizeof(res));
        NGAstNode* opt = task.ast;
        NGLuaStats lstats;
        memset(&lstats, 0, sizeof(lstats));
#ifdef NG_HAS_EMBEDDED_LUA
        if (g_dv.L && ng_lua_optimize_ast(g_dv.L, task.ast, &lstats, &opt)) {
            res.stats.folds = lstats.folds;
            res.stats.dce = lstats.dce;
            res.stats.alg = lstats.alg;
            res.stats.functions = lstats.functions;
            res.stats.globals = lstats.globals;
            res.stats.loops = lstats.loops;
            res.stats.calls = lstats.calls;
        } else
#endif
        {
            opt = optimize_node(task.ast);
        }
        if (!opt) opt = task.ast;

        res.ast = opt;
        res.filepath = task.filepath;
        res.seq = task.seq;
        res.success = (opt != NULL);
        res.stats.elapsed_ms = get_time_ms() - t0;

        /* Geração do IR pelo Core 1 (armazena observáveis no L2) */
        if (opt) {
            NGLIRNode* ir = ng_ir_from_ast(opt);
            size_t bc_len = 0;
            uint8_t* bc = ng_ir_to_bytecode(ir, &bc_len);
            ng_l2_set_bytecode(bc, bc_len);
            ng_l2_set_ir(ir, ng_ir_node_count(ir));
            collect_ir_symbols(ir);
        }

        atomic_fetch_add(&g_dv.stats.tasks_completed, 1);
        if (!ng_l2_push_result(&res)) {
            /* Fila de resultados cheia: Core 0 fará fallback; libera AST local */
            if (res.ast && res.ast != task.ast) ng_ast_free(res.ast);
        }
        g_dv.core1_state = NG_DV_STATE_IDLE;
    }
    g_dv.core1_state = NG_DV_STATE_STOPPED;
    return NULL;
}

/* ---------------- Lifecycle ---------------- */

void ng_dynamicv_init(void) {
    if (g_dv.initialized) return;
    memset(&g_dv, 0, sizeof(g_dv));
    g_dv.initialized = true;
    g_dv.enabled = true;

    const char* env_dis = getenv("NG_DYNAMICV_DISABLE");
    if (env_dis && (strcmp(env_dis, "1") == 0 || strcmp(env_dis, "true") == 0)) g_dv.enabled = false;
    const char* env_dv = getenv("NG_DYNAMICV");
    if (env_dv && (strcmp(env_dv, "0") == 0 || strcmp(env_dv, "false") == 0)) g_dv.enabled = false;

    ng_l2_init();

#ifdef NG_HAS_EMBEDDED_LUA
    init_embedded_lua();
#endif

    if (g_dv.enabled) {
        g_dv.shutdown_requested = false;
        if (pthread_create(&g_dv.worker_thread, NULL, core1_worker_loop, NULL) == 0) {
            g_dv.worker_running = true;
        } else {
            g_dv.worker_running = false;
        }
    }
    g_dv.core0_state = NG_DV_STATE_IDLE;
    g_dv.core1_state = g_dv.worker_running ? NG_DV_STATE_IDLE : NG_DV_STATE_FALLBACK;
}

void ng_dynamicv_cleanup(void) {
    if (!g_dv.initialized) return;
    if (g_dv.worker_running) {
        atomic_store(&g_dv.shutdown_requested, true);
        NGL2Task sentinel;
        memset(&sentinel, 0, sizeof(sentinel));
        ng_l2_push_task(&sentinel);
        ng_l2_shutdown_queues();
        pthread_join(g_dv.worker_thread, NULL);
        g_dv.worker_running = false;
    }
    ng_l2_shutdown_queues();
#ifdef NG_HAS_EMBEDDED_LUA
    if (g_dv.L) {
        lua_close(g_dv.L);
        g_dv.L = NULL;
    }
#endif
    ng_l2_shutdown();
    g_dv.initialized = false;
    g_dv.core0_state = NG_DV_STATE_STOPPED;
    g_dv.core1_state = NG_DV_STATE_STOPPED;
}

bool ng_dynamicv_is_enabled(void) { return g_dv.enabled; }
void ng_dynamicv_set_enabled(bool enabled) { g_dv.enabled = enabled; }

NGDynamicVState ng_dynamicv_get_core0_state(void) { return g_dv.core0_state; }
NGDynamicVState ng_dynamicv_get_core1_state(void) { return g_dv.core1_state; }

const NGDynamicVStats* ng_dynamicv_get_stats(void) { return &g_dv.stats; }

/* ---------------- Pipeline Dual-Core (Miss/Hit) ---------------- */

NGAstNode* ng_dynamicv_optimize_ast(NGAstNode* ast) {
    if (!ast) return NULL;
    if (!g_dv.initialized) ng_dynamicv_init();

    if (g_dv.worker_running) {
        NGL2Task task;
        task.ast = ast;
        task.filepath = NULL;
        task.seq = ++g_dv.next_seq;
        if (ng_l2_push_task(&task)) {
            double deadline = get_time_ms() + NG_DV_RESULT_WAIT_MS;
            while (get_time_ms() < deadline) {
                NGL2Result res;
                bool timed_out = false;
                if (!ng_l2_pop_result_timeout(&res, NG_DV_POP_MS, &timed_out)) {
                    if (timed_out) break;
                    continue;
                }
                if (res.seq == task.seq && res.ast) {
                    NGAstNode* out = res.ast;
                    if (out != ast) ng_ast_free(ast);
                    return out;
                }
            }
        }
    }
    atomic_fetch_add(&g_dv.stats.fallbacks_to_core0, 1);
    return optimize_node(ast);
}

NGAstNode* ng_dynamicv_prepare_program(const char* filepath, const char* source,
                                       bool store_compiled, bool* out_error) {
    if (!g_dv.initialized) ng_dynamicv_init();
    *out_error = false;
    g_dv.core0_state = NG_DV_STATE_EXECUTING;

    /* 1. Consulta ao N3 Cache (HIT) */
    NGAstNode* cached_program = n3_cache_load_ast(filepath, source);
    if (cached_program) {
        atomic_fetch_add(&g_dv.stats.hits, 1);
        return cached_program;
    }

    /* 2. Rota de MISS */
    atomic_fetch_add(&g_dv.stats.misses, 1);

    NGLexer lexer;
    ng_lexer_init(&lexer, source, filepath);
    NGParser parser;
    ng_parser_init(&parser, &lexer, filepath);
    NGAstNode* program = ng_parser_parse_program(&parser);
    ng_parser_cleanup(&parser);

    if (parser.has_error || !program) {
        if (program) ng_ast_free(program);
        *out_error = true;
        return NULL;
    }

    NGAstNode* original = program;
    uint64_t seq = ++g_dv.next_seq;

    /* 3. Despacho para o Core 1 via L2 (barramento C11) */
    if (g_dv.worker_running) {
        NGL2Task task;
        task.ast = program;
        task.filepath = filepath;
        task.seq = seq;
        if (ng_l2_push_task(&task)) {
            atomic_fetch_add(&g_dv.stats.tasks_dispatched, 1);
            ng_l2_meta_set("program_filepath", filepath);
            double deadline = get_time_ms() + NG_DV_RESULT_WAIT_MS;
            bool got = false;
            while (!g_dv.shutdown_requested && get_time_ms() < deadline) {
                NGL2Result res;
                bool timed_out = false;
                if (!ng_l2_pop_result_timeout(&res, NG_DV_POP_MS, &timed_out)) {
                    if (timed_out) break;
                    continue;
                }
                if (res.seq == seq && res.ast) {
                    program = res.ast;
                    if (program != original) ng_ast_free(original);
                    atomic_fetch_add(&g_dv.stats.constant_folds, (uint64_t)res.stats.folds);
                    atomic_fetch_add(&g_dv.stats.dead_code_eliminations, (uint64_t)res.stats.dce);
                    atomic_fetch_add(&g_dv.stats.algebraic_simplifications, (uint64_t)res.stats.alg);
                    atomic_fetch_add(&g_dv.stats.optimizations_applied,
                                     (uint64_t)(res.stats.folds + res.stats.dce + res.stats.alg +
                                                res.stats.functions + res.stats.globals +
                                                res.stats.loops + res.stats.calls));
                    atomic_fetch_add(&g_dv.stats.ir_generated, 1);
                    atomic_store(&g_dv.stats.core1_time_ms,
                                 atomic_load(&g_dv.stats.core1_time_ms) + res.stats.elapsed_ms);
                    got = true;
                    break;
                }
                /* Resultado antigo/inválido: ignora (Worker já acabou de levar). */
            }
            if (!got) {
                atomic_fetch_add(&g_dv.stats.fallbacks_to_core0, 1);
                program = original;
            }
        } else {
            atomic_fetch_add(&g_dv.stats.fallbacks_to_core0, 1);
            program = optimize_node(original);
        }
    } else {
        atomic_fetch_add(&g_dv.stats.fallbacks_to_core0, 1);
        program = optimize_node(original);
    }
    /* 4. Atualiza snapshots de memória do L2 no estado do motor */
    g_dv.stats.l2_current_bytes = ng_l2_current_bytes();
    g_dv.stats.l2_peak_bytes = ng_l2_peak_bytes();

    /* 5. Salva no N3 Cache (escrita atômica) */
    if (store_compiled && program) {
        n3_cache_store_ast(filepath, source, program);
    }

    g_dv.core0_state = NG_DV_STATE_IDLE;
    return program;
}

/* ---------------- CLI: status | cache | limpar | diagnostico ---------------- */

void ng_dynamicv_status(FILE* out) {
    if (!out) out = stdout;
    ng_dynamicv_init();
    const NGDynamicVStats* s = &g_dv.stats;
    const NGL2Stats* l2s = ng_l2_stats();
    fprintf(out, "====================================================\n");
    fprintf(out, " NG Dynamic V Dual-Core Status\n");
    fprintf(out, "====================================================\n");
    fprintf(out, "Status do Motor:      %s\n", g_dv.enabled ? "ATIVADO" : "DESATIVADO");
    fprintf(out, "Core 0 (C Runtime):   %s\n", state_str(g_dv.core0_state));
    fprintf(out, "Core 1 (Lua Worker):  %s (Worker thread: %s)\n",
            state_str(g_dv.core1_state), g_dv.worker_running ? "SIM" : "NAO (Fallback)");
#ifdef NG_HAS_EMBEDDED_LUA
    fprintf(out, "Lua Embedded C11:     SIM (liblua5.4 via liblua)\n");
#else
    fprintf(out, "Lua Embedded C11:     NAO (modo C11 autônomo)\n");
#endif
    fprintf(out, "----------------------------------------------------\n");
    fprintf(out, "Métricas do Pipeline e Cache L2/N3:\n");
    fprintf(out, "  Cache Hits:                   %llu\n", (unsigned long long)atomic_load(&s->hits));
    fprintf(out, "  Cache Misses:                 %llu\n", (unsigned long long)atomic_load(&s->misses));
    fprintf(out, "  Tarefas Despachadas p/ Core1: %llu\n", (unsigned long long)atomic_load(&s->tasks_dispatched));
    fprintf(out, "  Tarefas Concluídas no Core1:  %llu\n", (unsigned long long)atomic_load(&s->tasks_completed));
    fprintf(out, "  Fallbacks p/ Core 0:          %llu\n", (unsigned long long)atomic_load(&s->fallbacks_to_core0));
    fprintf(out, "  Otimizações Totais:           %llu\n", (unsigned long long)atomic_load(&s->optimizations_applied));
    fprintf(out, "    - Constant Folds:           %llu\n", (unsigned long long)atomic_load(&s->constant_folds));
    fprintf(out, "    - Dead Code Eliminations:   %llu\n", (unsigned long long)atomic_load(&s->dead_code_eliminations));
    fprintf(out, "    - Simplificações Algébricas:%llu\n", (unsigned long long)atomic_load(&s->algebraic_simplifications));
    fprintf(out, "  Tempo de CPU do Core 1:       %.3f ms\n", atomic_load(&s->core1_time_ms));
    fprintf(out, "  IRs Geradas pelo Core 1:      %llu\n", (unsigned long long)atomic_load(&s->ir_generated));
    fprintf(out, "----------------------------------------------------\n");
    fprintf(out, "L2 (RAM) Barramento:\n");
    fprintf(out, "  Tarefas push/pop:             %llu / %llu\n",
            (unsigned long long)atomic_load(&l2s->tasks_pushed),
            (unsigned long long)atomic_load(&l2s->tasks_popped));
    fprintf(out, "  Resultados push/pop:          %llu / %llu\n",
            (unsigned long long)atomic_load(&l2s->results_pushed),
            (unsigned long long)atomic_load(&l2s->results_popped));
    fprintf(out, "  Sync (waits/timeouts):        %llu / %llu\n",
            (unsigned long long)atomic_load(&l2s->sync_waits),
            (unsigned long long)atomic_load(&l2s->sync_timeouts));
    fprintf(out, "  Memória: current=%zu peak=%zu bytes\n",
            ng_l2_current_bytes(), ng_l2_peak_bytes());
    fprintf(out, "  Símbolos: %zu\n", ng_l2_symbols_count());
    for (size_t i = 0; i < ng_l2_symbols_count() && i < 8; i++) {
        fprintf(out, "    - %s\n", ng_l2_symbol_at(i));
    }
    fprintf(out, "====================================================\n");
}

void ng_dynamicv_cache_info(FILE* out) {
    if (!out) out = stdout;
    size_t count = n3_cache_count_entries();
    size_t bytes = n3_cache_total_bytes();
    const NGL2Stats* l2s = ng_l2_stats();
    fprintf(out, "====================================================\n");
    fprintf(out, " NG N3 Cache Inspeção\n");
    fprintf(out, "====================================================\n");
    fprintf(out, "Diretório de Cache:   %s\n", N3_CACHE_DIR);
    fprintf(out, "Total de V-units:     %zu / %d (limite máximo)\n", count, N3_CACHE_MAX_ENTRIES);
    fprintf(out, "Ocupação em Disco:    %zu bytes (%.2f KB)\n", bytes, (double)bytes / 1024.0);
    fprintf(out, "Modo de Gravação:     Atômica via rename temporário\n");
    fprintf(out, "Integridade:          Magic 'N3C1' + Hash SHA-256 + Versão\n");
    fprintf(out, "----------------------------------------------------\n");
    fprintf(out, "L2: IR=%zu bytes, símbolos=%zu, metadados=%zu\n",
            g_dv.stats.l2_peak_bytes, ng_l2_symbols_count(), 0UL);
    fprintf(out, "Bus: push=%llu pop=%llu waits=%llu timeouts=%llu\n",
            (unsigned long long)atomic_load(&l2s->tasks_pushed),
            (unsigned long long)atomic_load(&l2s->tasks_popped),
            (unsigned long long)atomic_load(&l2s->sync_waits),
            (unsigned long long)atomic_load(&l2s->sync_timeouts));
    fprintf(out, "====================================================\n");
}

void ng_dynamicv_clean_cache(void) {
    n3_cache_clean();
    ng_l2_clear_observables();
    atomic_store(&g_dv.stats.hits, 0);
    atomic_store(&g_dv.stats.misses, 0);
    printf("[Dynamic V] N3 Cache limpo e estatísticas zeradas com sucesso.\n");
}

void ng_dynamicv_diagnostico(FILE* out) {
    if (!out) out = stdout;
    ng_dynamicv_init();
    const NGDynamicVStats* s = &g_dv.stats;
    const NGL2Stats* l2s = ng_l2_stats();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long rss_kb = (long)(ru.ru_maxrss / 1024);
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0) nproc = 1;

    fprintf(out, "====================================================\n");
    fprintf(out, " NG Dynamic V Diagnóstico de Ambiente\n");
    fprintf(out, "====================================================\n");
    fprintf(out, "Versão NG:            %s\n", NG_VERSION);
    fprintf(out, "Barramento POSIX:     pthread_mutex + pthread_cond ativo\n");
    fprintf(out, "Atomics C11:          sim (contadores estatísticos)\n");
    fprintf(out, "Fila circular:        tarefas (%d) + resultados (%d)\n",
            NG_L2_TASK_CAP, NG_L2_RESULT_CAP);
#ifdef NG_HAS_EMBEDDED_LUA
    fprintf(out, "Suporte Lua Embedded: DETECTADO E VINCULADO\n");
    if (g_dv.L) fprintf(out, "Estado Lua (Core 1):  lua_State alocado com sucesso\n");
#else
    fprintf(out, "Suporte Lua Embedded: DESATIVADO NO BUILD (fallback C)\n");
#endif
    fprintf(out, "Diretório N3 Cache:   %s\n", N3_CACHE_DIR);
    fprintf(out, "PID do Processo:      %d\n", (int)getpid());
    fprintf(out, "Threads:              worker=%s, CPUs=%ld\n",
            g_dv.worker_running ? "SIM" : "NAO", nproc);
    fprintf(out, "Memória Processo:     maxRSS=%ld KB\n", rss_kb);
    fprintf(out, "L2: current=%zu peak=%zu bytes\n", ng_l2_current_bytes(), ng_l2_peak_bytes());
    fprintf(out, "Bus: push=%llu pop=%llu waits=%llu timeouts=%llu\n",
            (unsigned long long)atomic_load(&l2s->tasks_pushed),
            (unsigned long long)atomic_load(&l2s->tasks_popped),
            (unsigned long long)atomic_load(&l2s->sync_waits),
            (unsigned long long)atomic_load(&l2s->sync_timeouts));
    fprintf(out, "N3: entradas=%zu bytes=%zu\n", n3_cache_count_entries(), n3_cache_total_bytes());
    fprintf(out, "Otimizações: folds=%llu dce=%llu alg=%llu ir_gen=%llu\n",
            (unsigned long long)atomic_load(&s->constant_folds),
            (unsigned long long)atomic_load(&s->dead_code_eliminations),
            (unsigned long long)atomic_load(&s->algebraic_simplifications),
            (unsigned long long)atomic_load(&s->ir_generated));
    fprintf(out, "====================================================\n");
    ng_dynamicv_bench(out);
}

/* ---------------- Benchmark ---------------- */

static const char* BENCH_SRCS[] = {
    "Total = 0\nEnquanto Total < 100\n    Total = Total + 1\nFim\n",
    "A = 3 * 4 + 5\nB = A * 2\nC = B - A\nD = C / 2\nE = D * 0 + 1\nPrint E\n",
};
#define BENCH_N_SRCS 2
#define BENCH_REPS 10

void ng_dynamicv_bench(FILE* out) {
    if (!out) out = stdout;
    fprintf(out, "====================================================\n");
    fprintf(out, " Benchmark Dynamic V (miss vs hit)\n");
    fprintf(out, "====================================================\n");
    for (int s = 0; s < BENCH_N_SRCS; s++) {
        const char* path = ".ng_dynv_bench";
        double miss_total = 0.0, hit_total = 0.0;
        int miss_ok = 0, hit_ok = 0;
        for (int i = 0; i < BENCH_REPS; i++) {
            n3_cache_remove_entry(path);
            bool err = false;
            double t0 = get_time_ms();
            NGAstNode* p = ng_dynamicv_prepare_program(path, BENCH_SRCS[s], true, &err);
            miss_total += get_time_ms() - t0;
            miss_ok += (!err && p != NULL);
            ng_ast_free(p);
        }
        for (int i = 0; i < BENCH_REPS; i++) {
            bool err = false;
            double t0 = get_time_ms();
            NGAstNode* p = ng_dynamicv_prepare_program(path, BENCH_SRCS[s], true, &err);
            hit_total += get_time_ms() - t0;
            hit_ok += (!err && p != NULL);
            ng_ast_free(p);
        }
        fprintf(out, "Cenário %d:\n", s + 1);
        fprintf(out, "  Miss (parse+Core1): %.3f ms/it  ok=%d/%d\n",
                miss_total / BENCH_REPS, miss_ok, BENCH_REPS);
        fprintf(out, "  Hit  (N3 Load):     %.3f ms/it  ok=%d/%d\n",
                hit_total / BENCH_REPS, hit_ok, BENCH_REPS);
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    fprintf(out, "Memória Processo: maxRSS=%ld KB\n", (long)(ru.ru_maxrss / 1024));
    fprintf(out, "Flag desativação: NG_DYNAMICV=0 / NG_DYNAMICV_DISABLE=1\n");
    fprintf(out, "====================================================\n");
}
