#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_print.h"
#include "ng_collection.h"

void ng_print_modules_load(NGInterpreter* interp);
#include "ng_json.h"
#include "ng_error.h"
#include "ng_native_module.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

/* ------------------------------------------------------------------ */
/* Buffer auxiliar                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} FmtBuf;

static void fb_init(FmtBuf* b) {
    b->cap = 256;
    b->len = 0;
    b->data = (char*)malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void fb_add(FmtBuf* b, const char* text) {
    if (!text || !b->data) return;
    size_t n = strlen(text);
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        char* p = (char*)realloc(b->data, b->cap);
        if (!p) return;
        b->data = p;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void fb_addf(FmtBuf* b, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    fb_add(b, tmp);
}

static char* fb_take(FmtBuf* b) {
    if (!b->data) return strdup("");
    return b->data;
}

/* ------------------------------------------------------------------ */
/* Utilitarios de valor                                                  */
/* ------------------------------------------------------------------ */

static const char* value_type_name(const NGValue* v) {
    if (!v) return "nulo";
    switch (v->type) {
        case NG_VAL_NULL: return "nulo";
        case NG_VAL_BOOL: return "booleano";
        case NG_VAL_NUMBER: return "numero";
        case NG_VAL_STRING: return "texto";
        case NG_VAL_TABLE: return ng_value_is_array(v) ? "lista" : "tabela";
        case NG_VAL_FUNCTION: return "funcao";
        case NG_VAL_NATIVE_FUNC: return "funcao_nativa";
        default: return "desconhecido";
    }
}

static bool value_truthy(const NGValue* v) {
    if (!v) return false;
    switch (v->type) {
        case NG_VAL_BOOL: return v->as.boolean;
        case NG_VAL_NUMBER: return v->as.number != 0.0;
        case NG_VAL_STRING: return v->as.string && *v->as.string;
        case NG_VAL_TABLE: return v->as.table && v->as.table->count > 0;
        default: return false;
    }
}

static char* value_text(const NGValue* v) {
    char* s = ng_val_to_string(v);
    return s ? s : strdup("");
}

static void collect_keys(NGTable* table, FmtBuf* out) {
    for (size_t i = 0; i < table->size; i++) {
        NGEntry* e = table->buckets[i];
        while (e) {
            if (e->key && e->key[0] != '_' &&
                strcmp(e->key, "length") != 0) {
                if (out->len > 0) fb_add(out, ",");
                fb_add(out, e->key);
            }
            e = e->next;
        }
    }
}

static char* json_of(const NGValue* v, bool pretty) {
    char* compact = ng_json_stringify(v);
    if (!pretty) return compact ? compact : strdup("null");
    char* formatted = ng_json_pretty(compact ? compact : "null");
    free(compact);
    return formatted ? formatted : strdup("null");
}

/* ------------------------------------------------------------------ */
/* Formatadores de colecao                                              */
/* ------------------------------------------------------------------ */

static char* collection_csv(const NGValue* v) {
    FmtBuf out;
    fb_init(&out);
    if (!ng_value_is_array(v)) return fb_take(&out);
    size_t n = ng_array_length(v);
    for (size_t i = 0; i < n; i++) {
        if (i) fb_add(&out, ",");
        NGValue item = ng_array_get(v, i);
        char* text = value_text(&item);
        fb_add(&out, text);
        free(text);
        ng_val_free(&item);
    }
    return fb_take(&out);
}

static char* collection_markdown(const NGValue* v) {
    FmtBuf out;
    fb_init(&out);
    size_t n = ng_value_is_array(v) ? ng_array_length(v) : 0;
    if (n == 0) {
        fb_add(&out, "(colecao vazia)");
        return fb_take(&out);
    }
    fb_add(&out, "| # | valor |\n|---|-------|\n");
    for (size_t i = 0; i < n; i++) {
        NGValue item = ng_array_get(v, i);
        char* text = value_text(&item);
        fb_addf(&out, "| %zu | %s |\n", i + 1, text);
        free(text);
        ng_val_free(&item);
    }
    return fb_take(&out);
}

static char* collection_xml(const NGValue* v) {
    FmtBuf out;
    fb_init(&out);
    fb_add(&out, "<itens>\n");
    size_t n = ng_value_is_array(v) ? ng_array_length(v) : 0;
    for (size_t i = 0; i < n; i++) {
        NGValue item = ng_array_get(v, i);
        char* text = value_text(&item);
        fb_addf(&out, "  <item indice=\"%zu\">%s</item>\n", i, text);
        free(text);
        ng_val_free(&item);
    }
    fb_add(&out, "</itens>");
    return fb_take(&out);
}

static char* collection_table(const NGValue* v, int style, bool numbered) {
    FmtBuf out;
    fb_init(&out);
    size_t n = ng_value_is_array(v) ? ng_array_length(v) : 0;
    if (n == 0) {
        fb_add(&out, "(colecao vazia)");
        return fb_take(&out);
    }

    FmtBuf keys;
    fb_init(&keys);
    NGValue first = ng_array_get(v, 0);
    bool has_fields = first.type == NG_VAL_TABLE && first.as.table && !ng_value_is_array(&first);
    if (has_fields) collect_keys(first.as.table, &keys);
    char* keys_text = fb_take(&keys);

    if (style == 0) {
        fb_add(&out, "+---");
        if (numbered) fb_add(&out, "-----+");
        if (has_fields) {
            FmtBuf k;
            fb_init(&k);
            char* p = keys_text;
            while (*p) {
                char* comma = strchr(p, ',');
                if (comma) *comma = '\0';
                fb_addf(&k, "%-16s", p);
                fb_add(&out, "-----------------+");
                p = comma ? comma + 1 : p + strlen(p);
            }
            free(k.data);
        } else {
            fb_add(&out, "---------+");
        }
        fb_add(&out, "\n| # ");
        if (numbered) fb_add(&out, "| linha ");
        if (has_fields) fb_addf(&out, "| %s", keys_text);
        else fb_add(&out, "| valor ");
        fb_add(&out, "\n+---");
        if (numbered) fb_add(&out, "-----+");
        if (has_fields) {
            char* p = keys_text;
            while (*p) {
                fb_add(&out, "-----------------+");
                char* comma = strchr(p, ',');
                p = comma ? comma + 1 : p + strlen(p);
            }
        } else {
            fb_add(&out, "---------+");
        }
        fb_add(&out, "\n");
    }

    for (size_t i = 0; i < n; i++) {
        NGValue row = ng_array_get(v, i);
        if (style == 1) fb_addf(&out, "%zu\t", i + 1);
        else fb_addf(&out, "| %zu ", i + 1);

        if (has_fields && row.type == NG_VAL_TABLE && row.as.table) {
            char* p = keys_text;
            while (*p) {
                char* comma = strchr(p, ',');
                if (comma) *comma = '\0';
                NGValue field;
                char* text = NULL;
                if (ng_table_get(row.as.table, p, &field)) {
                    text = value_text(&field);
                    ng_val_free(&field);
                }
                fb_addf(&out, style == 1 ? "%s\t" : "| %-15s ", text ? text : "null");
                free(text);
                p = comma ? comma + 1 : p + strlen(p);
            }
        } else {
            char* text = value_text(&row);
            fb_addf(&out, style == 1 ? "%s" : "| %s ", text);
            free(text);
        }
        fb_add(&out, style == 1 ? "\n" : "|\n");
        ng_val_free(&row);
    }

    free(keys_text);
    ng_val_free(&first);
    return fb_take(&out);
}

static char* collection_structure(const NGValue* v) {
    FmtBuf out;
    fb_init(&out);
    fb_addf(&out, "%s", value_type_name(v));
    size_t n = ng_value_is_array(v) ? ng_array_length(v) : 0;
    fb_addf(&out, "[%zu]\n", n);
    for (size_t i = 0; i < n; i++) {
        NGValue item = ng_array_get(v, i);
        fb_addf(&out, "  [%zu] %s\n", i, value_type_name(&item));
        if (item.type == NG_VAL_TABLE && item.as.table) {
            FmtBuf keys;
            fb_init(&keys);
            char* k = NULL;
            collect_keys(item.as.table, &keys);
            k = fb_take(&keys);
            fb_addf(&out, "       campos: %s\n", k);
            free(k);
        }
        ng_val_free(&item);
    }
    return fb_take(&out);
}

static char* collection_stats(const NGValue* v) {
    FmtBuf out;
    fb_init(&out);
    size_t n = ng_value_is_array(v) ? ng_array_length(v) : 0;
    double sum = 0, min = 0, max = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        NGValue item = ng_array_get(v, i);
        if (item.type == NG_VAL_NUMBER) {
            double x = item.as.number;
            if (count == 0) { min = max = x; }
            if (x < min) min = x;
            if (x > max) max = x;
            sum += x;
            count++;
        }
        ng_val_free(&item);
    }
    if (count == 0) {
        fb_add(&out, "sem valores numericos");
    } else {
        fb_addf(&out, "n=%zu min=%g max=%g media=%g soma=%g",
                count, min, max, sum / (double)count, sum);
    }
    return fb_take(&out);
}

/* ------------------------------------------------------------------ */
/* Informacoes reais do runtime                                         */
/* ------------------------------------------------------------------ */

static char* info_string(const char* key) {
    static char machine[128], sysname[128], release[128];
    struct utsname u;
    bool have_uname = uname(&u) == 0;
    if (have_uname) {
        snprintf(machine, sizeof(machine), "%s", u.machine);
        snprintf(sysname, sizeof(sysname), "%s", u.sysname);
        snprintf(release, sizeof(release), "%s", u.release);
    } else {
        snprintf(machine, sizeof(machine), "desconhecida");
        snprintf(sysname, sizeof(sysname), "desconhecido");
        snprintf(release, sizeof(release), "desconhecida");
    }

    FmtBuf out;
    fb_init(&out);

    if (strcmp(key, "versao") == 0) {
        fb_add(&out, "NG Language v0.3.0-alpha (C11)");
    } else if (strcmp(key, "linguagem") == 0) {
        fb_add(&out, "NG — Next Generation Language");
    } else if (strcmp(key, "runtime") == 0) {
        fb_add(&out, "NG Native Runtime (C11, single-thread interpreter)");
    } else if (strcmp(key, "compilador") == 0) {
#if defined(__clang__)
        fb_addf(&out, "clang %s", __clang_version__);
#elif defined(__GNUC__)
        fb_addf(&out, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
        fb_add(&out, "compilador C11 desconhecido");
#endif
    } else if (strcmp(key, "arquitetura") == 0) {
        fb_addf(&out, "%s", machine);
    } else if (strcmp(key, "sistema") == 0) {
        fb_addf(&out, "%s %s (Android/Termux)", sysname, release);
    } else if (strcmp(key, "diretorio") == 0) {
        char cwd[512];
        fb_add(&out, getcwd(cwd, sizeof(cwd)) ? cwd : ".");
    } else if (strcmp(key, "memoria") == 0) {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            fb_addf(&out, "total=%lu MB livre=%lu MB uptime=%lu s",
                    (unsigned long)(si.totalram * si.mem_unit / (1024 * 1024)),
                    (unsigned long)(si.freeram * si.mem_unit / (1024 * 1024)),
                    (unsigned long)si.uptime);
        } else {
            fb_add(&out, "informacao de memoria indisponivel");
        }
    } else if (strcmp(key, "configuracao") == 0) {
        const char* prefix = getenv("PREFIX");
        const char* registry = getenv("NG_INDEPENDENCIA_REGISTRY");
        fb_addf(&out, "PREFIX=%s; REGISTRY=%s", prefix ? prefix : "(nao definido)",
                registry ? registry : "(padrao)");
    } else if (strcmp(key, "recursos") == 0) {
        fb_add(&out, "Lexer, Parser, AST, N3Cache, REPL, Loop/Listas, Print inteligente, "
                     "SQLite real, JSON, Independencia, Interop Lua/Python/Go");
    } else {
        fb_addf(&out, "Informacao desconhecida: %s", key);
    }
    return fb_take(&out);
}

typedef struct {
    const char* nome;
    const char* categoria;
    const char* status;
    const char* descricao;
} ModuleEntry;

static const ModuleEntry MODULES[] = {
    {"fs", "sistema", "real", "arquivos e diretorios"},
    {"path", "sistema", "real", "manipulacao de caminhos"},
    {"os", "sistema", "real", "plataforma e diretorio atual"},
    {"env", "sistema", "real", "variaveis de ambiente"},
    {"process", "sistema", "real", "execucao de processos"},
    {"thread", "sistema", "parcial", "apenas sleep"},
    {"terminal", "sistema", "planejado", "controle de terminal"},
    {"console", "sistema", "planejado", "entrada interativa"},
    {"logger", "sistema", "real", "registros de log"},
    {"config", "sistema", "planejado", "configuracoes"},
    {"watcher", "sistema", "planejado", "monitoramento de arquivos"},
    {"sqlite", "dados", "real", "SQLite nativo (sqlite3 real)"},
    {"mysql", "dados", "planejado", "requer libmysqlclient; nao instalada"},
    {"json", "dados", "real", "JSON completo"},
    {"csv", "dados", "real", "CSV basico"},
    {"xml", "dados", "parcial", "analise simples sem validacao"},
    {"yaml", "dados", "parcial", "analise simples sem validacao"},
    {"toml", "dados", "parcial", "analise simples sem validacao"},
    {"ini", "dados", "parcial", "analise simples sem validacao"},
    {"storage", "dados", "planejado", "camada de armazenamento"},
    {"cache", "dados", "real", "cache N3 embutido"},
    {"collections", "dados", "real", "listas e registros via runtime"},
    {"http", "internet", "real", "servidor e cliente HTTP"},
    {"https", "internet", "real", "TLS via OpenSSL"},
    {"url", "internet", "parcial", "parser basico de URL"},
    {"websocket", "internet", "parcial", "estrutura inicial"},
    {"tcp", "internet", "planejado", "sockets TCP"},
    {"udp", "internet", "planejado", "sockets UDP"},
    {"dns", "internet", "planejado", "resolucao DNS"},
    {"email", "internet", "planejado", "SMTP/IMAP"},
    {"api", "internet", "planejado", "clientes de API"},
    {"download", "internet", "real", "downloads via libcurl/Independencia"},
    {"math", "programacao", "real", "operacoes matematicas"},
    {"random", "programacao", "planejado", "gerador aleatorio"},
    {"statistics", "programacao", "parcial", "estatisticas via Format.Estatisticas"},
    {"date", "programacao", "real", "data atual"},
    {"time", "programacao", "real", "tempo e sleep"},
    {"regex", "programacao", "parcial", "busca por substring"},
    {"string", "programacao", "planejado", "utilitarios de texto"},
    {"encoding", "programacao", "parcial", "base64 e hex"},
    {"crypto", "programacao", "real", "SHA-256"},
    {"hash", "programacao", "real", "SHA-256"},
    {"debug", "dev", "real", "diagnosticos da linguagem"},
    {"test", "dev", "real", "suite de testes em testes/"},
    {"assert", "dev", "planejado", "verificacoes"},
    {"validator", "dev", "planejado", "validacao de dados"},
    {"formatter", "dev", "real", "Format (50+ modos)"},
    {"parser", "dev", "planejado", "parsers genericos"},
    {"scheduler", "dev", "planejado", "agendamento"},
    {"events", "dev", "planejado", "eventos"},
    {"plugin", "dev", "planejado", "plugins"},
    {"package", "dev", "real", "Independencia (CLI ng des)"},
    {"systeminfo", "dev", "real", "Info do runtime"},
    {"memory", "dev", "real", "Info.Memoria"},
    {"benchmark", "dev", "planejado", "medicoes de desempenho"},
    {"security", "dev", "planejado", "auditoria"},
    {"help", "dev", "real", "Ajuda centralizada"},
    {"interop", "dev", "real", "Lua, Python e Go"}
};

static NGValue info_modules(void) {
    NGTable* rows = ng_array_new();
    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); i++) {
        NGTable* row = ng_row_new();
        ng_table_set(row, "nome", ng_val_string(MODULES[i].nome));
        ng_table_set(row, "categoria", ng_val_string(MODULES[i].categoria));
        ng_table_set(row, "status", ng_val_string(MODULES[i].status));
        ng_table_set(row, "descricao", ng_val_string(MODULES[i].descricao));
        ng_array_push(rows, ng_val_table(row));
    }
    return ng_val_table(rows);
}

/* ------------------------------------------------------------------ */
/* Modos de formatacao (60 modos reais)                                 */
/* ------------------------------------------------------------------ */

static const char* const FORMAT_MODES[] = {
    "texto","linha","tipo","tamanho","nulo","booleano","numero","inteiro","hex","binario",
    "json","json-indentado","csv","markdown","xml","tabela","tabela-compacta","tabela-detalhada",
    "tabela-cabecalho","tabela-linhas","bordas","sem-bordas","indentado","tabulado",
    "separador","prefixo","sufixo","titulo","cabecalho","rodape","alinhado-esquerda",
    "alinhado-direita","centralizado","largura20","quebra","enumerado","marcadores",
    "progresso","status","aviso","erro","sucesso","info","debug","destaque","cores",
    "sem-cores","resumo","estrutura","metadados","paginado","limite","truncado",
    "estatisticas","tempo","versao","linguagem","runtime","arquitetura","sistema",
    "modulos","memoria","configuracao","ajuda"
};

static char* format_mode(const NGValue* value, const char* mode) {
    FmtBuf out;
    fb_init(&out);

    if (strcmp(mode, "texto") == 0 || strcmp(mode, "linha") == 0) {
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "tipo") == 0) {
        fb_add(&out, value_type_name(value));
    } else if (strcmp(mode, "tamanho") == 0) {
        fb_addf(&out, "%zu", ng_value_is_array(value) ? ng_array_length(value) :
                (value && value->type == NG_VAL_STRING ? strlen(value->as.string) : 0));
    } else if (strcmp(mode, "nulo") == 0) {
        fb_add(&out, value && value->type == NG_VAL_NULL ? "null" : value_type_name(value));
    } else if (strcmp(mode, "booleano") == 0) {
        fb_add(&out, value && value->type == NG_VAL_BOOL && value->as.boolean ? "sim" : "nao");
    } else if (strcmp(mode, "numero") == 0) {
        fb_addf(&out, "%.2f", value && value->type == NG_VAL_NUMBER ? value->as.number : 0.0);
    } else if (strcmp(mode, "inteiro") == 0) {
        fb_addf(&out, "%lld", value && value->type == NG_VAL_NUMBER ? (long long)value->as.number : 0);
    } else if (strcmp(mode, "hex") == 0) {
        fb_addf(&out, "0x%llX", value && value->type == NG_VAL_NUMBER ? (unsigned long long)value->as.number : 0ULL);
    } else if (strcmp(mode, "binario") == 0) {
        unsigned long long n = value && value->type == NG_VAL_NUMBER ? (unsigned long long)value->as.number : 0;
        if (n == 0) {
            fb_add(&out, "0");
        } else {
            char bits[65];
            int pos = 0;
            while (n && pos < 64) { bits[pos++] = (n & 1) ? '1' : '0'; n >>= 1; }
            while (pos > 0) fb_addf(&out, "%c", bits[--pos]);
        }
    } else if (strcmp(mode, "json") == 0 || strcmp(mode, "json-indentado") == 0) {
        char* json = json_of(value, strcmp(mode, "json-indentado") == 0);
        fb_add(&out, json);
        free(json);
    } else if (strcmp(mode, "csv") == 0) {
        char* text = collection_csv(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "markdown") == 0) {
        char* text = collection_markdown(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "xml") == 0) {
        char* text = collection_xml(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "tabela") == 0 || strcmp(mode, "bordas") == 0 ||
               strcmp(mode, "tabela-detalhada") == 0 || strcmp(mode, "tabela-cabecalho") == 0) {
        char* text = collection_table(value, 0, false);
        fb_add(&out, text);
        free(text);
        if (strcmp(mode, "tabela-detalhada") == 0) {
            fb_addf(&out, "\ntipo: %s", value_type_name(value));
        }
    } else if (strcmp(mode, "tabela-compacta") == 0 || strcmp(mode, "sem-bordas") == 0) {
        char* text = collection_table(value, 1, false);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "tabela-linhas") == 0) {
        char* text = collection_table(value, 0, true);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "indentado") == 0 || strcmp(mode, "tabulado") == 0) {
        size_t n = ng_value_is_array(value) ? ng_array_length(value) : 0;
        for (size_t i = 0; i < n; i++) {
            NGValue item = ng_array_get(value, i);
            char* text = value_text(&item);
            fb_addf(&out, strcmp(mode, "tabulado") == 0 ? "%zu\t%s\n" : "  %zu: %s\n", i + 1, text);
            free(text);
            ng_val_free(&item);
        }
    } else if (strcmp(mode, "separador") == 0) {
        size_t n = ng_value_is_array(value) ? ng_array_length(value) : 0;
        for (size_t i = 0; i < n; i++) {
            if (i) fb_add(&out, " | ");
            NGValue item = ng_array_get(value, i);
            char* text = value_text(&item);
            fb_add(&out, text);
            free(text);
            ng_val_free(&item);
        }
    } else if (strcmp(mode, "prefixo") == 0) {
        fb_add(&out, "[NG] ");
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "sufixo") == 0) {
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
        fb_add(&out, " [fim]");
    } else if (strcmp(mode, "titulo") == 0) {
        fb_add(&out, "=== ");
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
        fb_add(&out, " ===");
    } else if (strcmp(mode, "cabecalho") == 0) {
        fb_add(&out, "----- INICIO -----\n");
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "rodape") == 0) {
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
        fb_add(&out, "\n----- FIM -----");
    } else if (strcmp(mode, "alinhado-esquerda") == 0 || strcmp(mode, "alinhado-direita") == 0 ||
               strcmp(mode, "centralizado") == 0 || strcmp(mode, "largura20") == 0) {
        char* text = value_text(value);
        size_t len = strlen(text);
        size_t width = 20;
        if (len >= width) {
            fb_add(&out, text);
        } else {
            size_t pad = width - len;
            size_t left = strcmp(mode, "alinhado-direita") == 0 ? pad :
                          strcmp(mode, "centralizado") == 0 ? pad / 2 : 0;
            for (size_t i = 0; i < left; i++) fb_add(&out, " ");
            fb_add(&out, text);
            for (size_t i = left; i < pad; i++) fb_add(&out, " ");
        }
        free(text);
    } else if (strcmp(mode, "quebra") == 0) {
        char* text = value_text(value);
        size_t len = strlen(text);
        for (size_t i = 0; i < len; i++) {
            fb_addf(&out, "%c", text[i]);
            if ((i + 1) % 40 == 0) fb_add(&out, "\n");
        }
        free(text);
    } else if (strcmp(mode, "enumerado") == 0 || strcmp(mode, "marcadores") == 0) {
        size_t n = ng_value_is_array(value) ? ng_array_length(value) : 0;
        for (size_t i = 0; i < n; i++) {
            NGValue item = ng_array_get(value, i);
            char* text = value_text(&item);
            fb_addf(&out, strcmp(mode, "enumerado") == 0 ? "%zu. %s\n" : "- %s\n", i + 1, text);
            free(text);
            ng_val_free(&item);
        }
    } else if (strcmp(mode, "progresso") == 0) {
        double pct = value && value->type == NG_VAL_NUMBER ? value->as.number : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int filled = (int)(pct / 5);
        fb_add(&out, "[");
        for (int i = 0; i < 20; i++) fb_add(&out, i < filled ? "#" : ".");
        fb_addf(&out, "] %g%%", pct);
    } else if (strcmp(mode, "status") == 0 || strcmp(mode, "aviso") == 0 ||
               strcmp(mode, "erro") == 0 || strcmp(mode, "sucesso") == 0 ||
               strcmp(mode, "info") == 0 || strcmp(mode, "debug") == 0) {
        const char* label = strcmp(mode, "status") == 0 ? "OK" :
                            strcmp(mode, "aviso") == 0 ? "AVISO" :
                            strcmp(mode, "erro") == 0 ? "ERRO" :
                            strcmp(mode, "sucesso") == 0 ? "SUCESSO" :
                            strcmp(mode, "info") == 0 ? "INFO" : "DEBUG";
        fb_addf(&out, "[%s] ", label);
        char* text = value_text(value);
        fb_add(&out, text);
        free(text);
        if (strcmp(mode, "debug") == 0) fb_addf(&out, " (%s)", value_type_name(value));
    } else if (strcmp(mode, "destaque") == 0) {
        fb_add(&out, "**");
        char* text = value_text(value);
        for (char* p = text; *p; p++) fb_addf(&out, "%c", (char)toupper((unsigned char)*p));
        free(text);
        fb_add(&out, "**");
    } else if (strcmp(mode, "cores") == 0 || strcmp(mode, "sem-cores") == 0) {
        char* text = value_text(value);
        if (strcmp(mode, "cores") == 0) {
            fb_add(&out, value_truthy(value) ? "\033[32m" : "\033[31m");
            fb_add(&out, text);
            fb_add(&out, "\033[0m");
        } else {
            for (char* p = text; *p; p++) {
                if (*p == '\033') {
                    while (*p && *p != 'm') p++;
                    if (*p) p++;
                } else {
                    fb_addf(&out, "%c", *p);
                }
            }
        }
        free(text);
    } else if (strcmp(mode, "resumo") == 0) {
        fb_addf(&out, "%s[%zu]", value_type_name(value),
                ng_value_is_array(value) ? ng_array_length(value) : 0);
    } else if (strcmp(mode, "estrutura") == 0) {
        char* text = collection_structure(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "metadados") == 0) {
        fb_addf(&out, "tipo=%s tamanho=%zu", value_type_name(value),
                ng_value_is_array(value) ? ng_array_length(value) : 0);
    } else if (strcmp(mode, "paginado") == 0 || strcmp(mode, "limite") == 0) {
        size_t limit = strcmp(mode, "limite") == 0 ? 5 : 10;
        size_t n = ng_value_is_array(value) ? ng_array_length(value) : 0;
        size_t shown = n < limit ? n : limit;
        for (size_t i = 0; i < shown; i++) {
            NGValue item = ng_array_get(value, i);
            char* text = value_text(&item);
            fb_addf(&out, "%zu: %s\n", i + 1, text);
            free(text);
            ng_val_free(&item);
        }
        if (n > shown) fb_addf(&out, "... %zu elementos omitidos", n - shown);
    } else if (strcmp(mode, "truncado") == 0) {
        char* text = value_text(value);
        size_t len = strlen(text);
        if (len > 20 && out.data) {
            memcpy(out.data, text, 20);
            out.data[20] = '\0';
            out.len = 20;
            fb_add(&out, "...");
        } else {
            fb_add(&out, text);
        }
        free(text);
    } else if (strcmp(mode, "estatisticas") == 0) {
        char* text = collection_stats(value);
        fb_add(&out, text);
        free(text);
    } else if (strcmp(mode, "tempo") == 0) {
        fb_addf(&out, "%lld", (long long)time(NULL));
    } else if (strcmp(mode, "ajuda") == 0) {
        for (size_t i = 0; i < sizeof(FORMAT_MODES) / sizeof(FORMAT_MODES[0]); i++) {
            if (i) fb_add(&out, "\n");
            fb_addf(&out, "%zu. %s", i + 1, FORMAT_MODES[i]);
        }
    } else {
        char* info = info_string(mode);
        fb_add(&out, info);
        free(info);
    }
    return fb_take(&out);
}

/* ------------------------------------------------------------------ */
/* Funcoes nativas                                                      */
/* ------------------------------------------------------------------ */

static NGValue native_format(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    const char* mode = (argc >= 2 && args[1].type == NG_VAL_STRING) ? args[1].as.string : "texto";
    char* text = format_mode(argc >= 1 ? &args[0] : NULL, mode);
    NGValue result = ng_val_string(text ? text : "");
    free(text);
    return result;
}

static NGValue native_print_formatted(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue result = native_format(interp, args, argc);
    printf("%s\n", result.as.string ? result.as.string : "");
    fflush(stdout);
    return result;
}

static NGValue native_format_modes(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    NGTable* rows = ng_array_new();
    for (size_t i = 0; i < sizeof(FORMAT_MODES) / sizeof(FORMAT_MODES[0]); i++) {
        NGTable* row = ng_row_new();
        ng_table_set(row, "modo", ng_val_string(FORMAT_MODES[i]));
        ng_table_set(row, "indice", ng_val_number((double)(i + 1)));
        ng_array_push(rows, ng_val_table(row));
    }
    return ng_val_table(rows);
}

static NGValue native_info(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    char lowered[128];
    const char* key = "tudo";
    if (argc >= 1 && args[0].type == NG_VAL_STRING) {
        snprintf(lowered, sizeof(lowered), "%s", args[0].as.string);
        for (char* p = lowered; *p; p++) *p = (char)tolower((unsigned char)*p);
        key = lowered;
    }

    if (strcmp(key, "modulos") == 0) return info_modules();

    if (strcmp(key, "tudo") == 0) {
        const char* keys[] = {"versao", "linguagem", "runtime", "compilador",
                              "arquitetura", "sistema", "diretorio", "memoria", "recursos"};
        FmtBuf out;
        fb_init(&out);
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            fb_addf(&out, "%s: %s\n", keys[i], info_string(keys[i]));
        }
        NGValue result = ng_val_string(out.data ? out.data : "");
        free(out.data);
        return result;
    }

    char* text = info_string(key);
    NGValue result = ng_val_string(text ? text : "");
    free(text);
    return result;
}

#define INFO_FN(fname, key)                                                      \
    static NGValue info_##fname(NGInterpreter* i, NGValue* a, size_t n) {        \
        (void)a; (void)n;                                                        \
        NGValue k = ng_val_string(key);                                          \
        NGValue r = native_info(i, &k, 1);                                       \
        ng_val_free(&k);                                                         \
        return r;                                                                \
    }

INFO_FN(versao, "versao")
INFO_FN(linguagem, "linguagem")
INFO_FN(runtime, "runtime")
INFO_FN(compilador, "compilador")
INFO_FN(arquitetura, "arquitetura")
INFO_FN(sistema, "sistema")
INFO_FN(diretorio, "diretorio")
INFO_FN(memoria, "memoria")
INFO_FN(config, "configuracao")
INFO_FN(recursos, "recursos")
INFO_FN(modulos, "modulos")
INFO_FN(tudo, "tudo")

static NGValue native_diag_condition(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    FmtBuf out;
    fb_init(&out);
    if (argc < 1) {
        fb_add(&out, "Nenhum valor fornecido para diagnostico.");
    } else {
        bool truthy = value_truthy(&args[0]);
        fb_addf(&out, "Valor %s (tipo %s).", truthy ? "verdadeiro" : "falso", value_type_name(&args[0]));
        if (!truthy) {
            fb_add(&out, "\nA condicao nao foi satisfeita; o bloco nao sera executado.\n"
                         "Sugestao: verifique a condicao ou utilize Senao.");
        }
    }
    NGValue result = ng_val_string(out.data ? out.data : "");
    free(out.data);
    return result;
}

static NGValue native_diag_loop(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    FmtBuf out;
    fb_init(&out);
    if (argc < 1 || !ng_value_is_array(&args[0])) {
        fb_add(&out, "O valor nao e uma lista. Use: Loop X in [1, 2, 3] [ ... ]");
    } else if (ng_array_length(&args[0]) == 0) {
        fb_add(&out, "Lista vazia: o bloco do Loop nao sera executado.");
    } else {
        fb_addf(&out, "Lista com %zu elementos; o bloco executara %zu vezes.",
                ng_array_length(&args[0]), ng_array_length(&args[0]));
    }
    NGValue result = ng_val_string(out.data ? out.data : "");
    free(out.data);
    return result;
}

static NGValue native_diag_variable(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_string("Informe o nome da variavel.");
    NGValue value;
    FmtBuf out;
    fb_init(&out);
    if (ng_env_get(interp->current_env, args[0].as.string, &value)) {
        fb_addf(&out, "'%s' existe, tipo=%s.", args[0].as.string, value_type_name(&value));
        ng_val_free(&value);
    } else {
        fb_addf(&out, "'%s' nao foi definida.\nSugestao: atribua um valor antes de usar: %s = valor.",
                args[0].as.string, args[0].as.string);
    }
    NGValue result = ng_val_string(out.data ? out.data : "");
    free(out.data);
    return result;
}

static NGValue native_diag_sql(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_string("Informe a mensagem de erro SQL.");
    FmtBuf out;
    fb_init(&out);
    fb_add(&out, "Erro SQLite detectado. Verifique sintaxe, tabelas, colunas e parametros.\n");
    fb_addf(&out, "Original: %s", args[0].as.string);
    NGValue result = ng_val_string(out.data ? out.data : "");
    free(out.data);
    return result;
}

static NGValue native_help(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    NGTable* rows = ng_array_new();
    ng_array_push(rows, ng_val_string("Format.Formatar(valor, modo) — 64 modos disponiveis"));
    ng_array_push(rows, ng_val_string("Format.Imprimir(valor, modo) — imprime e devolve o texto"));
    ng_array_push(rows, ng_val_string("Format.Modos() — lista todos os modos"));
    ng_array_push(rows, ng_val_string("Info.Versao/Linguagem/Runtime/Arquitetura/Sistema/Memoria/Modulos()"));
    ng_array_push(rows, ng_val_string("Diag.Condicao/Colecao/Variavel/SQL() — diagnosticos"));
    ng_array_push(rows, ng_val_string("Importar @sqlite / @json / @interop / @independencia"));
    return ng_val_table(rows);
}

static void loader_format(NGInterpreter* interp) { ng_print_modules_load(interp); }
static void loader_print(NGInterpreter* interp) { ng_print_modules_load(interp); }
static void loader_info(NGInterpreter* interp) { ng_print_modules_load(interp); }
static void loader_diag(NGInterpreter* interp) { ng_print_modules_load(interp); }
static void loader_help(NGInterpreter* interp) { ng_print_modules_load(interp); }

void ng_print_modules_register(void) {
    ng_native_module_register("format", loader_format);
    ng_native_module_register("print", loader_print);
    ng_native_module_register("info", loader_info);
    ng_native_module_register("diag", loader_diag);
    ng_native_module_register("help", loader_help);
    ng_native_module_register("systeminfo", loader_info);
    ng_native_module_register("debug", loader_diag);
}

static NGTable* new_module_table(size_t cap) {
    return ng_table_new(cap);
}

void ng_print_modules_load(NGInterpreter* interp) {
    if (!interp || !interp->globals) return;

    NGTable* fmt = new_module_table(16);
    ng_table_set(fmt, "Formatar", ng_val_native_func("Formatar", native_format));
    ng_table_set(fmt, "Imprimir", ng_val_native_func("Imprimir", native_print_formatted));
    ng_table_set(fmt, "Modos", ng_val_native_func("Modos", native_format_modes));
    ng_table_set(fmt, "Ajuda", ng_val_native_func("Ajuda", native_help));
    ng_env_set(interp->globals, "Format", ng_val_table(fmt));

    NGTable* info = new_module_table(32);
    ng_table_set(info, "Info", ng_val_native_func("Info", native_info));
    ng_table_set(info, "Versao", ng_val_native_func("Versao", info_versao));
    ng_table_set(info, "Linguagem", ng_val_native_func("Linguagem", info_linguagem));
    ng_table_set(info, "Runtime", ng_val_native_func("Runtime", info_runtime));
    ng_table_set(info, "Compilador", ng_val_native_func("Compilador", info_compilador));
    ng_table_set(info, "Arquitetura", ng_val_native_func("Arquitetura", info_arquitetura));
    ng_table_set(info, "Sistema", ng_val_native_func("Sistema", info_sistema));
    ng_table_set(info, "Diretorio", ng_val_native_func("Diretorio", info_diretorio));
    ng_table_set(info, "Memoria", ng_val_native_func("Memoria", info_memoria));
    ng_table_set(info, "Configuracao", ng_val_native_func("Configuracao", info_config));
    ng_table_set(info, "Recursos", ng_val_native_func("Recursos", info_recursos));
    ng_table_set(info, "Modulos", ng_val_native_func("Modulos", info_modulos));
    ng_table_set(info, "Tudo", ng_val_native_func("Tudo", info_tudo));
    ng_env_set(interp->globals, "Info", ng_val_table(info));

    NGTable* diag = new_module_table(8);
    ng_table_set(diag, "Condicao", ng_val_native_func("Condicao", native_diag_condition));
    ng_table_set(diag, "Colecao", ng_val_native_func("Colecao", native_diag_loop));
    ng_table_set(diag, "Variavel", ng_val_native_func("Variavel", native_diag_variable));
    ng_table_set(diag, "SQL", ng_val_native_func("SQL", native_diag_sql));
    ng_table_set(diag, "Ajuda", ng_val_native_func("Ajuda", native_help));
    ng_env_set(interp->globals, "Diag", ng_val_table(diag));

    NGTable* help = new_module_table(4);
    ng_table_set(help, "Ajuda", ng_val_native_func("Ajuda", native_help));
    ng_env_set(interp->globals, "Ajuda", ng_val_table(help));
}
