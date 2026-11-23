#include <stdint.h>
#include "ng_n3cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <stdatomic.h>

#define N3_CACHE_DIR ".n3cache"

static _Atomic size_t g_n3_max_bytes = N3_CACHE_DEFAULT_MAX_BYTES;

void ng_sha256(const unsigned char* data, size_t len, unsigned char hash[32]) {
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    for (size_t i = 0; i < len; i++) {
        uint32_t v = (uint32_t)data[i];
        h0 = ((h0 << 5) | (h0 >> 27)) + v + 0x9e3779b9;
        h1 ^= (h0 >> 2) + (h1 << 6);
        h2 = (h2 + 0x7feb352d) ^ v;
        h3 = ((h3 << 7) | (h3 >> 25)) + h1;
        h4 ^= (h4 >> 3) + v;
        h5 = ((h5 << 11) | (h5 >> 21)) ^ h2;
        h6 += (h3 >> 5) + 0x85ebca6b;
        h7 ^= ((h7 << 13) | (h7 >> 19)) + h4;
    }
    uint32_t parts[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; i++) {
        hash[i*4 + 0] = (parts[i] >> 24) & 0xFF;
        hash[i*4 + 1] = (parts[i] >> 16) & 0xFF;
        hash[i*4 + 2] = (parts[i] >> 8) & 0xFF;
        hash[i*4 + 3] = parts[i] & 0xFF;
    }
}

void ng_sha256_hex(const unsigned char* data, size_t len, char hex_out[65]) {
    unsigned char h[32];
    ng_sha256(data, len, h);
    for (int i = 0; i < 32; i++) {
        sprintf(hex_out + (i * 2), "%02x", h[i]);
    }
    hex_out[64] = '\0';
}

static void ensure_cache_dir(void) {
    mkdir(N3_CACHE_DIR, 0755);
}

static void get_cache_path(const char* filepath, char* out_path, size_t max_len) {
    char hex[65];
    ng_sha256_hex((const unsigned char*)filepath, strlen(filepath), hex);
    snprintf(out_path, max_len, "%s/%.12s.n3c", N3_CACHE_DIR, hex);
}

bool n3_cache_is_valid(const char* filepath) {
    char cpath[256];
    get_cache_path(filepath, cpath, sizeof(cpath));
    FILE* f = fopen(cpath, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Conta entradas simples no diretorio do cache (para aplicar o limite). */
static size_t count_cache_entries(void) {
    DIR* d = opendir(N3_CACHE_DIR);
    if (!d) return 0;
    struct dirent* entry;
    size_t count = 0;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        count++;
    }
    closedir(d);
    return count;
}

bool n3_cache_store(const char* filepath, const char* compiled_blob, size_t size) {
    ensure_cache_dir();
    /* Limite simples de entradas: estoura o cache se atingir o maximo. */
    if (count_cache_entries() >= N3_CACHE_MAX_ENTRIES) {
        n3_cache_clean();
    }
    char cpath[256];
    get_cache_path(filepath, cpath, sizeof(cpath));

    /* Escrita atômica: grava em arquivo temporário e faz rename */
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s/tmp_%d_%ld.n3c", N3_CACHE_DIR, (int)getpid(), (long)rand());
    FILE* f = fopen(tmppath, "wb");
    if (!f) return false;
    if (fwrite(compiled_blob, 1, size, f) != size) {
        fclose(f);
        unlink(tmppath);
        return false;
    }
    fflush(f);
    fclose(f);
    if (rename(tmppath, cpath) != 0) {
        unlink(tmppath);
        return false;
    }
    /* Limite de bytes total do cache + retenção LRU (Dynamic V) */
    if (n3_cache_total_bytes() > n3_cache_get_max_bytes()) {
        n3_cache_sweep_retention();
    }
    if (n3_cache_total_bytes() > n3_cache_get_max_bytes() &&
        size > n3_cache_get_max_bytes()) {
        /* A própria entry excede o limite: invalida a entry */
        n3_cache_remove_entry(filepath);
        return false;
    }
    return true;
}

size_t n3_cache_count_entries(void) {
    return count_cache_entries();
}

size_t n3_cache_total_bytes(void) {
    DIR* d = opendir(N3_CACHE_DIR);
    if (!d) return 0;
    struct dirent* entry;
    char path[512];
    struct stat st;
    size_t total = 0;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", N3_CACHE_DIR, entry->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            total += (size_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

void n3_cache_remove_entry(const char* filepath) {
    char cpath[256];
    get_cache_path(filepath, cpath, sizeof(cpath));
    unlink(cpath);
}

void n3_cache_set_max_bytes(size_t max_bytes) { atomic_store(&g_n3_max_bytes, max_bytes); }
size_t n3_cache_get_max_bytes(void) { return atomic_load(&g_n3_max_bytes); }

typedef struct {
    char path[600];
    time_t mtime;
    size_t size;
} N3Entry;

static int n3_cmp_mtime(const void* a, const void* b) {
    const N3Entry* ea = (const N3Entry*)a;
    const N3Entry* eb = (const N3Entry*)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return 0;
}

void n3_cache_sweep_retention(void) {
    size_t max = n3_cache_get_max_bytes();
    if (n3_cache_total_bytes() <= max) return;
    DIR* d = opendir(N3_CACHE_DIR);
    if (!d) return;
    N3Entry* entries = NULL;
    size_t count = 0, cap = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || ent->d_name[0] == 't') continue; /* ignora "tmp_*" */
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", N3_CACHE_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            N3Entry* p = (N3Entry*)realloc(entries, cap * sizeof(N3Entry));
            if (!p) break;
            entries = p;
        }
        snprintf(entries[count].path, sizeof(entries[count].path), "%s", path);
        entries[count].mtime = st.st_mtime;
        entries[count].size = (size_t)st.st_size;
        count++;
    }
    closedir(d);
    qsort(entries, count, sizeof(N3Entry), n3_cmp_mtime);
    size_t total = n3_cache_total_bytes();
    for (size_t i = 0; i < count && total > max; i++) {
        if (unlink(entries[i].path) == 0) total -= entries[i].size;
    }
    free(entries);
}

bool n3_cache_entry_valid(const char* filepath) {
    char cpath[256];
    get_cache_path(filepath, cpath, sizeof(cpath));
    FILE* f = fopen(cpath, "rb");
    if (!f) return false;
    char magic[5] = {0};
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "N3C1", 4) == 0;
    fclose(f);
    return ok;
}

char* n3_cache_load(const char* filepath, size_t* out_size) {
    char cpath[256];
    get_cache_path(filepath, cpath, sizeof(cpath));
    FILE* f = fopen(cpath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

void n3_cache_clean(void) {
    DIR* d = opendir(N3_CACHE_DIR);
    if (!d) return;
    struct dirent* entry;
    char path[512];
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", N3_CACHE_DIR, entry->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(N3_CACHE_DIR);
}

typedef struct {
    char* data;
    size_t size;
    size_t cap;
} N3Buf;

static void buf_init(N3Buf* b) {
    b->cap = 1024;
    b->size = 0;
    b->data = (char*)malloc(b->cap);
}

static void buf_write(N3Buf* b, const void* src, size_t sz) {
    while (b->size + sz > b->cap) {
        b->cap *= 2;
        b->data = (char*)realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, src, sz);
    b->size += sz;
}

static void buf_write_u32(N3Buf* b, uint32_t val) {
    buf_write(b, &val, sizeof(uint32_t));
}

static void buf_write_double(N3Buf* b, double val) {
    buf_write(b, &val, sizeof(double));
}

static void buf_write_str(N3Buf* b, const char* str) {
    if (!str) {
        buf_write_u32(b, 0);
        return;
    }
    uint32_t len = (uint32_t)strlen(str);
    buf_write_u32(b, len);
    buf_write(b, str, len);
}

typedef struct {
    const char* data;
    size_t size;
    size_t cursor;
} N3Reader;

static bool r_read(N3Reader* r, void* dst, size_t sz) {
    if (r->cursor + sz > r->size) return false;
    memcpy(dst, r->data + r->cursor, sz);
    r->cursor += sz;
    return true;
}

static uint32_t r_u32(N3Reader* r) {
    uint32_t val = 0;
    r_read(r, &val, sizeof(uint32_t));
    return val;
}

static double r_double(N3Reader* r) {
    double val = 0.0;
    r_read(r, &val, sizeof(double));
    return val;
}

static char* r_str(N3Reader* r) {
    uint32_t len = r_u32(r);
    if (len == 0) return NULL;
    if (r->cursor + len > r->size) return NULL;
    char* s = (char*)malloc(len + 1);
    memcpy(s, r->data + r->cursor, len);
    s[len] = '\0';
    r->cursor += len;
    return s;
}

static void serialize_ast(N3Buf* b, const NGAstNode* node) {
    if (!node) {
        buf_write_u32(b, 0xFFFFFFFF);
        return;
    }
    buf_write_u32(b, (uint32_t)node->type);
    buf_write_u32(b, (uint32_t)node->line);
    buf_write_u32(b, (uint32_t)node->column);

    switch (node->type) {
        case NG_AST_PROGRAM:
            buf_write_u32(b, (uint32_t)node->as.program.count);
            for (size_t i = 0; i < node->as.program.count; i++) {
                serialize_ast(b, node->as.program.statements[i]);
            }
            break;
        case NG_AST_STMT_PRINT:
            serialize_ast(b, node->as.print_stmt.expr);
            break;
        case NG_AST_STMT_G:
            buf_write_u32(b, node->as.g_stmt.expr ? 1 : 0);
            if (node->as.g_stmt.expr) serialize_ast(b, node->as.g_stmt.expr);
            break;
        case NG_AST_STMT_Y:
            serialize_ast(b, node->as.y_stmt.expr);
            break;
        case NG_AST_STMT_ASSIGN:
            buf_write_str(b, node->as.assign_stmt.name);
            serialize_ast(b, node->as.assign_stmt.value);
            break;
        case NG_AST_STMT_IF:
            serialize_ast(b, node->as.if_stmt.condition);
            serialize_ast(b, node->as.if_stmt.then_block);
            serialize_ast(b, node->as.if_stmt.else_block);
            break;
        case NG_AST_STMT_WHILE:
            serialize_ast(b, node->as.while_stmt.condition);
            serialize_ast(b, node->as.while_stmt.block);
            break;
        case NG_AST_STMT_FUNC:
            buf_write_str(b, node->as.func_stmt.name);
            buf_write_u32(b, (uint32_t)node->as.func_stmt.param_count);
            for (size_t i = 0; i < node->as.func_stmt.param_count; i++) {
                buf_write_str(b, node->as.func_stmt.params[i]);
            }
            serialize_ast(b, node->as.func_stmt.body);
            break;
        case NG_AST_STMT_IMPORT:
            buf_write_str(b, node->as.import_stmt.module_name);
            buf_write_str(b, node->as.import_stmt.alias);
            break;
        case NG_AST_EXPR_BINARY:
            buf_write_u32(b, (uint32_t)node->as.binary_expr.op);
            serialize_ast(b, node->as.binary_expr.left);
            serialize_ast(b, node->as.binary_expr.right);
            break;
        case NG_AST_EXPR_LITERAL:
            buf_write_u32(b, (uint32_t)node->as.literal_expr.value.type);
            if (node->as.literal_expr.value.type == NG_VAL_NUMBER) {
                buf_write_double(b, node->as.literal_expr.value.as.number);
            } else if (node->as.literal_expr.value.type == NG_VAL_BOOL) {
                buf_write_u32(b, node->as.literal_expr.value.as.boolean ? 1 : 0);
            } else if (node->as.literal_expr.value.type == NG_VAL_STRING) {
                buf_write_str(b, node->as.literal_expr.value.as.string);
            }
            break;
        case NG_AST_EXPR_IDENTIFIER:
            buf_write_str(b, node->as.id_expr.name);
            break;
        case NG_AST_EXPR_CALL:
            serialize_ast(b, node->as.call_expr.callee);
            buf_write_u32(b, (uint32_t)node->as.call_expr.arg_count);
            for (size_t i = 0; i < node->as.call_expr.arg_count; i++) {
                serialize_ast(b, node->as.call_expr.args[i]);
            }
            break;
        case NG_AST_EXPR_TABLE_GET:
            serialize_ast(b, node->as.table_get.table);
            buf_write_str(b, node->as.table_get.field);
            break;
        case NG_AST_STMT_TABLE_SET:
            serialize_ast(b, node->as.table_set.table);
            buf_write_str(b, node->as.table_set.field);
            serialize_ast(b, node->as.table_set.value);
            break;
        case NG_AST_EXPR_FUNC:
            buf_write_str(b, node->as.func_expr.name);
            buf_write_u32(b, (uint32_t)node->as.func_expr.param_count);
            for (size_t i = 0; i < node->as.func_expr.param_count; i++) {
                buf_write_str(b, node->as.func_expr.params[i]);
            }
            serialize_ast(b, node->as.func_expr.body);
            break;
        case NG_AST_STMT_LOOP:
            buf_write_str(b, node->as.loop_stmt.var_name);
            serialize_ast(b, node->as.loop_stmt.iterable);
            serialize_ast(b, node->as.loop_stmt.body);
            break;
        case NG_AST_EXPR_ARRAY:
            buf_write_u32(b, (uint32_t)node->as.array_expr.count);
            for (size_t i = 0; i < node->as.array_expr.count; i++) {
                serialize_ast(b, node->as.array_expr.elements[i]);
            }
            break;
        case NG_AST_EXPR_INDEX:
            serialize_ast(b, node->as.index_expr.target);
            serialize_ast(b, node->as.index_expr.index);
            break;
        default:
            break;
    }
}

static NGAstNode* deserialize_ast(N3Reader* r) {
    uint32_t type_val = r_u32(r);
    if (type_val == 0xFFFFFFFF) return NULL;

    int line = (int)r_u32(r);
    int col = (int)r_u32(r);
    NGAstNode* node = ng_ast_new((NGAstNodeType)type_val, line, col);

    switch (node->type) {
        case NG_AST_PROGRAM: {
            uint32_t count = r_u32(r);
            node->as.program.count = count;
            node->as.program.capacity = count;
            node->as.program.statements = count ? (NGAstNode**)malloc(sizeof(NGAstNode*) * count) : NULL;
            for (uint32_t i = 0; i < count; i++) {
                node->as.program.statements[i] = deserialize_ast(r);
            }
            break;
        }
        case NG_AST_STMT_PRINT:
            node->as.print_stmt.expr = deserialize_ast(r);
            break;
        case NG_AST_STMT_G: {
            uint32_t has_e = r_u32(r);
            node->as.g_stmt.expr = has_e ? deserialize_ast(r) : NULL;
            break;
        }
        case NG_AST_STMT_Y:
            node->as.y_stmt.expr = deserialize_ast(r);
            break;
        case NG_AST_STMT_ASSIGN:
            node->as.assign_stmt.name = r_str(r);
            node->as.assign_stmt.value = deserialize_ast(r);
            break;
        case NG_AST_STMT_IF:
            node->as.if_stmt.condition = deserialize_ast(r);
            node->as.if_stmt.then_block = deserialize_ast(r);
            node->as.if_stmt.else_block = deserialize_ast(r);
            break;
        case NG_AST_STMT_WHILE:
            node->as.while_stmt.condition = deserialize_ast(r);
            node->as.while_stmt.block = deserialize_ast(r);
            break;
        case NG_AST_STMT_FUNC: {
            node->as.func_stmt.name = r_str(r);
            uint32_t pc = r_u32(r);
            node->as.func_stmt.param_count = pc;
            node->as.func_stmt.param_capacity = pc;
            node->as.func_stmt.params = pc ? (char**)malloc(sizeof(char*) * pc) : NULL;
            for (uint32_t i = 0; i < pc; i++) {
                node->as.func_stmt.params[i] = r_str(r);
            }
            node->as.func_stmt.body = deserialize_ast(r);
            break;
        }
        case NG_AST_STMT_IMPORT:
            node->as.import_stmt.module_name = r_str(r);
            node->as.import_stmt.alias = r_str(r);
            break;
        case NG_AST_EXPR_BINARY:
            node->as.binary_expr.op = (int)r_u32(r);
            node->as.binary_expr.left = deserialize_ast(r);
            node->as.binary_expr.right = deserialize_ast(r);
            break;
        case NG_AST_EXPR_LITERAL: {
            uint32_t vt = r_u32(r);
            if (vt == NG_VAL_NUMBER) {
                node->as.literal_expr.value = ng_val_number(r_double(r));
            } else if (vt == NG_VAL_BOOL) {
                node->as.literal_expr.value = ng_val_bool(r_u32(r) != 0);
            } else if (vt == NG_VAL_STRING) {
                char* s = r_str(r);
                node->as.literal_expr.value = ng_val_string(s ? s : "");
                free(s);
            } else {
                node->as.literal_expr.value = ng_val_null();
            }
            break;
        }
        case NG_AST_EXPR_IDENTIFIER:
            node->as.id_expr.name = r_str(r);
            break;
        case NG_AST_EXPR_CALL: {
            node->as.call_expr.callee = deserialize_ast(r);
            uint32_t ac = r_u32(r);
            node->as.call_expr.arg_count = ac;
            node->as.call_expr.arg_capacity = ac;
            node->as.call_expr.args = ac ? (NGAstNode**)malloc(sizeof(NGAstNode*) * ac) : NULL;
            for (uint32_t i = 0; i < ac; i++) {
                node->as.call_expr.args[i] = deserialize_ast(r);
            }
            break;
        }
        case NG_AST_EXPR_TABLE_GET:
            node->as.table_get.table = deserialize_ast(r);
            node->as.table_get.field = r_str(r);
            break;
        case NG_AST_STMT_TABLE_SET:
            node->as.table_set.table = deserialize_ast(r);
            node->as.table_set.field = r_str(r);
            node->as.table_set.value = deserialize_ast(r);
            break;
        case NG_AST_EXPR_FUNC: {
            node->as.func_expr.name = r_str(r);
            uint32_t pc = r_u32(r);
            node->as.func_expr.param_count = pc;
            node->as.func_expr.param_capacity = pc;
            node->as.func_expr.params = pc ? (char**)malloc(sizeof(char*) * pc) : NULL;
            for (uint32_t i = 0; i < pc; i++) {
                node->as.func_expr.params[i] = r_str(r);
            }
            node->as.func_expr.body = deserialize_ast(r);
            break;
        }
        case NG_AST_STMT_LOOP:
            node->as.loop_stmt.var_name = r_str(r);
            node->as.loop_stmt.iterable = deserialize_ast(r);
            node->as.loop_stmt.body = deserialize_ast(r);
            break;
        case NG_AST_EXPR_ARRAY: {
            uint32_t ac = r_u32(r);
            node->as.array_expr.count = ac;
            node->as.array_expr.capacity = ac;
            node->as.array_expr.elements = ac ? (NGAstNode**)malloc(sizeof(NGAstNode*) * ac) : NULL;
            for (uint32_t i = 0; i < ac; i++) {
                node->as.array_expr.elements[i] = deserialize_ast(r);
            }
            break;
        }
        case NG_AST_EXPR_INDEX:
            node->as.index_expr.target = deserialize_ast(r);
            node->as.index_expr.index = deserialize_ast(r);
            break;
        default:
            break;
    }
    return node;
}

bool n3_cache_store_ast(const char* filepath, const char* source, const NGAstNode* ast) {
    if (!filepath || !source || !ast) return false;
    char combined[strlen(source) + strlen(NG_VERSION) + 16];
    snprintf(combined, sizeof(combined), "%s:%s", NG_VERSION, source);
    char hash[65];
    ng_sha256_hex((const unsigned char*)combined, strlen(combined), hash);

    N3Buf b;
    buf_init(&b);
    buf_write(&b, "N3C1", 4);
    buf_write_str(&b, NG_VERSION);
    buf_write(&b, hash, 64);

    serialize_ast(&b, ast);

    bool ok = n3_cache_store(filepath, b.data, b.size);
    free(b.data);
    return ok;
}

NGAstNode* n3_cache_load_ast(const char* filepath, const char* source) {
    if (!filepath || !source) return NULL;
    size_t sz = 0;
    char* data = n3_cache_load(filepath, &sz);
    if (!data || sz < 4 + 4 + 64) {
        if (data) free(data);
        return NULL;
    }

    N3Reader r = { .data = data, .size = sz, .cursor = 0 };
    char magic[5] = {0};
    r_read(&r, magic, 4);
    if (memcmp(magic, "N3C1", 4) != 0) {
        free(data);
        n3_cache_remove_entry(filepath);
        return NULL;
    }

    char* ver = r_str(&r);
    if (!ver || strcmp(ver, NG_VERSION) != 0) {
        free(ver);
        free(data);
        n3_cache_remove_entry(filepath);
        return NULL;
    }
    free(ver);

    char cached_hash[65] = {0};
    r_read(&r, cached_hash, 64);

    char combined[strlen(source) + strlen(NG_VERSION) + 16];
    snprintf(combined, sizeof(combined), "%s:%s", NG_VERSION, source);
    char current_hash[65];
    ng_sha256_hex((const unsigned char*)combined, strlen(combined), current_hash);

    if (strcmp(cached_hash, current_hash) != 0) {
        free(data);
        /* Hash diferente significa que o fonte mudou -> invalida entrada */
        n3_cache_remove_entry(filepath);
        return NULL;
    }

    NGAstNode* ast = deserialize_ast(&r);
    free(data);
    return ast;
}
