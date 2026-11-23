#ifndef NG_NO_SQLITE
#include <sqlite3.h>
#endif

#include "ng_sqlite.h"
#include "ng_collection.h"
#include "ng_error.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NG_NO_SQLITE

static sqlite3* g_active_db = NULL;

/* ------------------------------------------------------------------ */
/* Auxiliares                                                           */
/* ------------------------------------------------------------------ */

static void set_db_value(NGTable* table, sqlite3* db) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)(uintptr_t)db);
    ng_table_set(table, "_db", ng_val_string(buffer));
}

static sqlite3* db_from_receiver(NGInterpreter* interp) {
    NGValue value;
    if (interp && interp->receiver_table &&
        ng_table_get(interp->receiver_table, "_db", &value) &&
        value.type == NG_VAL_STRING && value.as.string) {
        return (sqlite3*)(uintptr_t)strtoull(value.as.string, NULL, 10);
    }
    return NULL;
}

static void store_last_error(NGInterpreter* interp, const char* message) {
    if (interp && interp->receiver_table) {
        ng_table_set(interp->receiver_table, "_erro", ng_val_string(message ? message : ""));
    }
}

static NGValue sql_error(NGInterpreter* interp, sqlite3* db, const char* code,
                         const char* title, const char* detail) {
    char buffer[1200];
    const char* native = db ? sqlite3_errmsg(db) : "Nenhuma conexao SQLite ativa.";
    if (detail) {
        snprintf(buffer, sizeof(buffer), "%s\n\nSQLite: %s", detail, native);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", native);
    }
    store_last_error(interp, buffer);
    ng_error_print(code, title, "<sqlite>", 0, 0, buffer,
                   "Verifique a sintaxe SQL, os nomes de tabelas/colunas e os parametros.");
    return ng_val_null();
}

static bool bind_params(sqlite3_stmt* stmt, const NGValue* params,
                        NGInterpreter* interp, sqlite3* db, int first_index) {
    if (!params || !ng_value_is_array(params)) return true;

    size_t count = ng_array_length(params);
    for (size_t i = 0; i < count; i++) {
        NGValue item = ng_array_get(params, i);
        int index = first_index + (int)i;
        int rc = SQLITE_OK;

        switch (item.type) {
            case NG_VAL_NULL:
                rc = sqlite3_bind_null(stmt, index);
                break;
            case NG_VAL_BOOL:
                rc = sqlite3_bind_int(stmt, index, item.as.boolean ? 1 : 0);
                break;
            case NG_VAL_NUMBER:
                rc = sqlite3_bind_double(stmt, index, item.as.number);
                break;
            case NG_VAL_STRING:
                rc = sqlite3_bind_text(stmt, index, item.as.string ? item.as.string : "",
                                       -1, SQLITE_TRANSIENT);
                break;
            default:
                rc = sqlite3_bind_null(stmt, index);
                break;
        }
        ng_val_free(&item);
        if (rc != SQLITE_OK) {
            sql_error(interp, db, NG_ERR_SQL_PARAM, "Parametro SQL Invalido",
                      "Nao foi possivel associar um parametro a consulta preparada.");
            return false;
        }
    }
    return true;
}

static NGValue value_from_column(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
        case SQLITE_INTEGER:
            return ng_val_number((double)sqlite3_column_int64(stmt, column));
        case SQLITE_FLOAT:
            return ng_val_number(sqlite3_column_double(stmt, column));
        case SQLITE_TEXT: {
            const unsigned char* text = sqlite3_column_text(stmt, column);
            return ng_val_string(text ? (const char*)text : "");
        }
        case SQLITE_BLOB: {
            const void* blob = sqlite3_column_blob(stmt, column);
            int bytes = sqlite3_column_bytes(stmt, column);
            if (!blob || bytes <= 0) return ng_val_string("");
            char* copy = (char*)malloc((size_t)bytes + 1);
            if (!copy) return ng_val_null();
            memcpy(copy, blob, (size_t)bytes);
            copy[bytes] = '\0';
            NGValue result = ng_val_string(copy);
            free(copy);
            return result;
        }
        case SQLITE_NULL:
        default:
            return ng_val_null();
    }
}

static NGValue rows_from_statement(sqlite3_stmt* stmt) {
    NGTable* rows = ng_array_new();
    if (!rows) return ng_val_null();

    int columns = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        NGTable* row = ng_row_new();
        for (int c = 0; c < columns; c++) {
            const char* name = sqlite3_column_name(stmt, c);
            if (!name) name = "coluna";
            ng_table_set(row, name, value_from_column(stmt, c));
        }
        ng_array_push(rows, ng_val_table(row));
    }
    return ng_val_table(rows);
}

static NGValue sqlite_execute_sql(NGInterpreter* interp, sqlite3* db,
                                  const char* sql, const NGValue* params) {
    if (!db || !sql || !*sql) {
        return sql_error(interp, db, NG_ERR_SQL_CONNECT, "Conexao SQLite Invalida",
                         "Abra um banco antes de executar SQL: Banco = SQLite(\"app.db\").");
    }

    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return sql_error(interp, db, NG_ERR_SQL, "Erro de SQL",
                         "Nao foi possivel preparar a instrucao SQL.");
    }

    if (!bind_params(stmt, params, interp, db, 1)) {
        sqlite3_finalize(stmt);
        return ng_val_null();
    }

    if (sqlite3_column_count(stmt) > 0) {
        NGValue rows = rows_from_statement(stmt);
        rc = sqlite3_errcode(db);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_OK && rc != SQLITE_DONE) {
            ng_val_free(&rows);
            return sql_error(interp, db, NG_ERR_SQL, "Erro de SQL",
                             "Ocorreu um erro durante a leitura dos resultados.");
        }
        return rows;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return sql_error(interp, db, NG_ERR_SQL, "Erro de SQL",
                         "Nao foi possivel executar a instrucao SQL.");
    }
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    return ng_val_number(changes);
}

static void quote_identifier(const char* name, char* out, size_t cap) {
    size_t pos = 0;
    out[pos++] = '"';
    for (const char* p = name; *p && pos + 3 < cap; p++) {
        if (*p == '"') {
            out[pos++] = '"';
            out[pos++] = '"';
        } else {
            out[pos++] = *p;
        }
    }
    if (pos + 1 < cap) out[pos++] = '"';
    out[pos] = '\0';
}

static NGValue require_string_arg(NGInterpreter* interp, NGValue* args, size_t argc,
                                  size_t index, const char* what) {
    if (argc <= index || args[index].type != NG_VAL_STRING) {
        char detail[256];
        snprintf(detail, sizeof(detail), "Era esperado um texto para %s.", what);
        return sql_error(interp, db_from_receiver(interp), NG_ERR_INVALID_ARGS,
                         "Argumento Invalido", detail);
    }
    return ng_val_null();
}

/* ------------------------------------------------------------------ */
/* Metodos da conexao                                                   */
/* ------------------------------------------------------------------ */

static NGValue native_sqlite_exec(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue bad = require_string_arg(interp, args, argc, 0, "a instrucao SQL");
    if (bad.type != NG_VAL_NULL) return bad;
    const NGValue* params = (argc > 1) ? &args[1] : NULL;
    return sqlite_execute_sql(interp, db_from_receiver(interp), args[0].as.string, params);
}

static NGValue native_sqlite_query(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue result = native_sqlite_exec(interp, args, argc);
    if (!ng_value_is_array(&result)) {
        NGTable* empty = ng_array_new();
        ng_val_free(&result);
        return ng_val_table(empty);
    }
    return result;
}

static NGValue native_sqlite_insert(NGInterpreter* interp, NGValue* args, size_t argc) {
    sqlite3* db = db_from_receiver(interp);
    NGValue bad = require_string_arg(interp, args, argc, 0, "o nome da tabela");
    if (bad.type != NG_VAL_NULL) return bad;
    if (argc < 2 || args[1].type != NG_VAL_TABLE || !args[1].as.table) {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Argumento Invalido",
                         "Inserir(tabela, registro) espera uma tabela de campos.");
    }

    NGTable* record = args[1].as.table;
    char names[1024] = {0};
    char marks[1024] = {0};
    NGValue* values[128];
    size_t count = 0;

    for (size_t i = 0; i < record->size && count < 128; i++) {
        NGEntry* entry = record->buckets[i];
        while (entry && count < 128) {
            if (entry->key && entry->key[0] != '_') {
                if (count > 0) {
                    strcat(names, ",");
                    strcat(marks, ",");
                }
                char quoted[256];
                quote_identifier(entry->key, quoted, sizeof(quoted));
                strncat(names, quoted, sizeof(names) - strlen(names) - 1);
                strcat(marks, "?");
                values[count++] = &entry->value;
            }
            entry = entry->next;
        }
    }
    if (count == 0) {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Registro Vazio",
                         "O registro nao possui campos para inserir.");
    }

    char table_name[256];
    quote_identifier(args[0].as.string, table_name, sizeof(table_name));
    char sql[2048];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s)", table_name, names, marks);

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return sql_error(interp, db, NG_ERR_SQL, "Erro ao Inserir", sql);
    }
    for (size_t i = 0; i < count; i++) {
        NGValue item = ng_val_clone(values[i]);
        int rc;
        switch (item.type) {
            case NG_VAL_BOOL: rc = sqlite3_bind_int(stmt, (int)i + 1, item.as.boolean ? 1 : 0); break;
            case NG_VAL_NUMBER: rc = sqlite3_bind_double(stmt, (int)i + 1, item.as.number); break;
            case NG_VAL_STRING: rc = sqlite3_bind_text(stmt, (int)i + 1, item.as.string ? item.as.string : "", -1, SQLITE_TRANSIENT); break;
            default: rc = sqlite3_bind_null(stmt, (int)i + 1); break;
        }
        ng_val_free(&item);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return sql_error(interp, db, NG_ERR_SQL_PARAM, "Parametro SQL Invalido", sql);
        }
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return sql_error(interp, db, NG_ERR_SQL, "Erro ao Inserir", sql);
    }
    sqlite3_int64 last_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return ng_val_number((double)last_id);
}

static NGValue native_sqlite_update(NGInterpreter* interp, NGValue* args, size_t argc) {
    sqlite3* db = db_from_receiver(interp);
    NGValue bad = require_string_arg(interp, args, argc, 0, "o nome da tabela");
    if (bad.type != NG_VAL_NULL) return bad;
    if (argc < 3 || args[2].type != NG_VAL_STRING) {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Argumento Invalido",
                         "Atualizar(tabela, registro, condicao) espera uma condicao WHERE.");
    }
    if (*args[2].as.string == '\0') {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Atualizacao Insegura",
                         "Atualizar sem WHERE alteraria todos os registros.");
    }

    NGTable* record = args[1].as.table;
    if (!record) {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Argumento Invalido",
                         "O registro deve ser uma tabela de campos.");
    }

    char sets[1200] = {0};
    NGValue* values[64];
    size_t count = 0;
    for (size_t i = 0; i < record->size && count < 64; i++) {
        NGEntry* entry = record->buckets[i];
        while (entry && count < 64) {
            if (entry->key && entry->key[0] != '_') {
                char quoted[256];
                quote_identifier(entry->key, quoted, sizeof(quoted));
                if (count > 0) strcat(sets, ",");
                strncat(sets, quoted, sizeof(sets) - strlen(sets) - 1);
                strcat(sets, "=?");
                values[count++] = &entry->value;
            }
            entry = entry->next;
        }
    }

    char table_name[256];
    quote_identifier(args[0].as.string, table_name, sizeof(table_name));
    char sql[2048];
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s WHERE %s", table_name, sets, args[2].as.string);

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return sql_error(interp, db, NG_ERR_SQL, "Erro ao Atualizar", sql);
    }
    size_t total = count + (argc > 3 ? ng_array_length(&args[3]) : 0);
    for (size_t i = 0; i < count; i++) {
        NGValue item = ng_val_clone(values[i]);
        int rc;
        switch (item.type) {
            case NG_VAL_BOOL: rc = sqlite3_bind_int(stmt, (int)i + 1, item.as.boolean ? 1 : 0); break;
            case NG_VAL_NUMBER: rc = sqlite3_bind_double(stmt, (int)i + 1, item.as.number); break;
            case NG_VAL_STRING: rc = sqlite3_bind_text(stmt, (int)i + 1, item.as.string ? item.as.string : "", -1, SQLITE_TRANSIENT); break;
            default: rc = sqlite3_bind_null(stmt, (int)i + 1); break;
        }
        ng_val_free(&item);
        (void)rc;
    }
    if (argc > 3 && !bind_params(stmt, &args[3], interp, db, (int)count + 1)) {
        sqlite3_finalize(stmt);
        return ng_val_null();
    }
    (void)total;

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return sql_error(interp, db, NG_ERR_SQL, "Erro ao Atualizar", sql);
    }
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    return ng_val_number(changes);
}

static NGValue native_sqlite_delete(NGInterpreter* interp, NGValue* args, size_t argc) {
    sqlite3* db = db_from_receiver(interp);
    NGValue bad = require_string_arg(interp, args, argc, 0, "o nome da tabela");
    if (bad.type != NG_VAL_NULL) return bad;

    const char* where = (argc > 1 && args[1].type == NG_VAL_STRING) ? args[1].as.string : "";
    bool confirmed = (argc > 3 && args[3].type == NG_VAL_STRING &&
                      strcmp(args[3].as.string, "confirmar") == 0);

    if (!*where && !confirmed) {
        return sql_error(interp, db, NG_ERR_INVALID_ARGS, "Exclusao Insegura",
                         "Excluir sem WHERE exige confirmacao: Excluir(tabela, \"\", parametros, \"confirmar\").");
    }

    char table_name[256];
    quote_identifier(args[0].as.string, table_name, sizeof(table_name));
    char sql[1024];
    if (*where) {
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s", table_name, where);
    } else {
        snprintf(sql, sizeof(sql), "DELETE FROM %s", table_name);
    }

    const NGValue* params = (argc > 2) ? &args[2] : NULL;
    NGValue result = sqlite_execute_sql(interp, db, sql, params);
    if (result.type == NG_VAL_NUMBER && result.as.number == 0 && *where) {
        ng_error_print(NG_ERR_SQL_EMPTY, "Nenhum Registro Excluido", "<sqlite>", 0, 0,
                       "A condicao WHERE nao correspondeu a nenhum registro.",
                       "Verifique os valores e a condicao informada.");
    }
    return result;
}

static NGValue native_sqlite_list_tables(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)args; (void)argc;
    return sqlite_execute_sql(interp, db_from_receiver(interp),
                              "SELECT name FROM sqlite_master WHERE type='table' "
                              "AND name NOT LIKE 'sqlite_%' ORDER BY name", NULL);
}

static NGValue native_sqlite_structure(NGInterpreter* interp, NGValue* args, size_t argc) {
    sqlite3* db = db_from_receiver(interp);
    NGValue bad = require_string_arg(interp, args, argc, 0, "o nome da tabela");
    if (bad.type != NG_VAL_NULL) return bad;
    char quoted[256];
    quote_identifier(args[0].as.string, quoted, sizeof(quoted));
    char sql[512];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", quoted);
    return sqlite_execute_sql(interp, db, sql, NULL);
}

static NGValue native_sqlite_columns(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue structure = native_sqlite_structure(interp, args, argc);
    if (!ng_value_is_array(&structure)) return structure;

    NGTable* names = ng_array_new();
    size_t count = ng_array_length(&structure);
    for (size_t i = 0; i < count; i++) {
        NGValue row = ng_array_get(&structure, i);
        NGValue name;
        if (row.type == NG_VAL_TABLE && row.as.table &&
            ng_table_get(row.as.table, "name", &name)) {
            /* ng_table_get devolve uma referencia interna: clona antes que a
               linha (e seus valores) seja liberada. */
            ng_array_push(names, ng_val_clone(&name));
        }
        ng_val_free(&row);
    }
    ng_val_free(&structure);
    return ng_val_table(names);
}

static NGValue native_sqlite_index(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue bad = require_string_arg(interp, args, argc, 0, "a SQL do indice");
    if (bad.type != NG_VAL_NULL) return bad;
    return sqlite_execute_sql(interp, db_from_receiver(interp), args[0].as.string, NULL);
}

static NGValue native_sqlite_transaction(NGInterpreter* interp, NGValue* args, size_t argc,
                                         const char* sql) {
    (void)args; (void)argc;
    return sqlite_execute_sql(interp, db_from_receiver(interp), sql, NULL);
}
static NGValue native_sqlite_begin(NGInterpreter* i, NGValue* a, size_t n) {
    return native_sqlite_transaction(i, a, n, "BEGIN TRANSACTION");
}
static NGValue native_sqlite_commit(NGInterpreter* i, NGValue* a, size_t n) {
    return native_sqlite_transaction(i, a, n, "COMMIT");
}
static NGValue native_sqlite_rollback(NGInterpreter* i, NGValue* a, size_t n) {
    return native_sqlite_transaction(i, a, n, "ROLLBACK");
}

static NGValue native_sqlite_close(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)args; (void)argc;
    sqlite3* db = db_from_receiver(interp);
    if (!db) return ng_val_bool(false);
    if (g_active_db == db) g_active_db = NULL;
    int rc = sqlite3_close(db);
    if (interp && interp->receiver_table) {
        set_db_value(interp->receiver_table, NULL);
        ng_table_set(interp->receiver_table, "_path", ng_val_string(""));
    }
    return ng_val_bool(rc == SQLITE_OK);
}

static NGValue native_sqlite_last_error(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)args; (void)argc;
    NGValue value;
    if (interp && interp->receiver_table &&
        ng_table_get(interp->receiver_table, "_erro", &value)) {
        return ng_val_clone(&value);
    }
    return ng_val_string("");
}

static NGValue native_sqlite_version(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_string(sqlite3_libversion());
}

static NGValue help_array(const char* const* lines, size_t count) {
    NGTable* rows = ng_array_new();
    for (size_t i = 0; i < count; i++) ng_array_push(rows, ng_val_string(lines[i]));
    return ng_val_table(rows);
}

static const char* const SQLITE_HELP[] = {
    "SQLite($arquivo)                    Abre/cria um banco .db/.sqlite/.sqlite3",
    "Executar($sql, $params)             Executa SQL (SELECT devolve colecao)",
    "Consultar($sql, $params)            Consulta e devolve colecao de registros",
    "Inserir($tabela, $registro)         Insercao com parametros preparados",
    "Atualizar($tabela, $registro, $where, $params)",
    "Excluir($tabela, $where, $params, $confirmar)",
    "ListarTabelas()                     Lista tabelas do banco",
    "Estrutura($tabela)                  Colunas, tipos e chaves primarias",
    "Colunas($tabela)                    Apenas nomes das colunas",
    "CriarIndice($sql)                   CREATE INDEX / DROP INDEX",
    "Iniciar() / Confirmar() / Desfazer() Transacoes",
    "list($comando, ...)                 Parametros: listar, criar tabela, inserir usuario,",
    "                                    consultar usuarios, estrutura, sql, ajuda",
    "Fechar()                            Fecha a conexao"
};

static NGValue native_sqlite_help(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return help_array(SQLITE_HELP, sizeof(SQLITE_HELP) / sizeof(SQLITE_HELP[0]));
}

static NGValue db_table_for_active(void) {
    NGTable* table = ng_table_new(8);
    set_db_value(table, g_active_db);
    ng_table_set(table, "_path", ng_val_string(""));
    return ng_val_table(table);
}

static NGValue native_sqlite_params(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) {
        NGTable* rows = ng_array_new();
        ng_array_push(rows, ng_val_string("Parametro ausente. Operacoes disponiveis:"));
        for (size_t i = 0; i < sizeof(SQLITE_HELP) / sizeof(SQLITE_HELP[0]); i++) {
            ng_array_push(rows, ng_val_string(SQLITE_HELP[i]));
        }
        return ng_val_table(rows);
    }

    const char* command = args[0].as.string;
    NGTable* db_table = ng_table_new(8);
    set_db_value(db_table, db_from_receiver(interp));
    ng_table_set(db_table, "_path", ng_val_string(""));

    NGInterpreter fake = *interp;
    fake.receiver_table = db_table;
    NGValue result;

    if (strstr(command, "listar") == command && strstr(command, "tabela") != NULL) {
        result = native_sqlite_list_tables(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "listar") == 0) {
        result = native_sqlite_list_tables(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "criar tabela") == 0) {
        result = native_sqlite_exec(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "inserir usuario") == 0 ||
               strcmp(command, "inserir") == 0) {
        result = native_sqlite_insert(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "consultar usuarios") == 0 ||
               strcmp(command, "consultar") == 0 || strcmp(command, "sql") == 0) {
        result = native_sqlite_exec(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "estrutura") == 0) {
        result = native_sqlite_structure(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "colunas") == 0) {
        result = native_sqlite_columns(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "iniciar") == 0) {
        result = native_sqlite_begin(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "confirmar") == 0) {
        result = native_sqlite_commit(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "desfazer") == 0) {
        result = native_sqlite_rollback(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "fechar") == 0) {
        result = native_sqlite_close(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "versao") == 0) {
        result = native_sqlite_version(&fake, args + 1, argc - 1);
    } else if (strcmp(command, "ajuda") == 0) {
        result = native_sqlite_help(&fake, args + 1, argc - 1);
    } else {
        NGTable* rows = ng_array_new();
        char message[256];
        snprintf(message, sizeof(message), "Parametro desconhecido: %s", command);
        ng_array_push(rows, ng_val_string(message));
        ng_array_push(rows, ng_val_string("Operacoes disponiveis:"));
        for (size_t i = 0; i < sizeof(SQLITE_HELP) / sizeof(SQLITE_HELP[0]); i++) {
            ng_array_push(rows, ng_val_string(SQLITE_HELP[i]));
        }
        result = ng_val_table(rows);
    }

    ng_table_free(db_table);
    return result;
}

static NGValue native_sqlite_open(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) {
        return sql_error(interp, NULL, NG_ERR_INVALID_ARGS, "Argumento Invalido",
                         "SQLite($arquivo) espera um caminho de arquivo.");
    }
    const char* path = args[0].as.string;
    sqlite3* db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        char detail[512];
        snprintf(detail, sizeof(detail), "Nao foi possivel abrir '%s'.", path);
        NGValue result = sql_error(interp, db, NG_ERR_SQL_CONNECT,
                                   "Falha ao Abrir Banco", detail);
        if (db) sqlite3_close(db);
        return result;
    }

    NGTable* connection = ng_table_new(24);
    set_db_value(connection, db);
    ng_table_set(connection, "_path", ng_val_string(path));
    ng_table_set(connection, "_erro", ng_val_string(""));

    ng_table_set(connection, "Executar", ng_val_native_func("Executar", native_sqlite_exec));
    ng_table_set(connection, "Consultar", ng_val_native_func("Consultar", native_sqlite_query));
    ng_table_set(connection, "Inserir", ng_val_native_func("Inserir", native_sqlite_insert));
    ng_table_set(connection, "Atualizar", ng_val_native_func("Atualizar", native_sqlite_update));
    ng_table_set(connection, "Excluir", ng_val_native_func("Excluir", native_sqlite_delete));
    ng_table_set(connection, "ListarTabelas", ng_val_native_func("ListarTabelas", native_sqlite_list_tables));
    ng_table_set(connection, "Estrutura", ng_val_native_func("Estrutura", native_sqlite_structure));
    ng_table_set(connection, "Colunas", ng_val_native_func("Colunas", native_sqlite_columns));
    ng_table_set(connection, "CriarIndice", ng_val_native_func("CriarIndice", native_sqlite_index));
    ng_table_set(connection, "CriarTabela", ng_val_native_func("CriarTabela", native_sqlite_exec));
    ng_table_set(connection, "Iniciar", ng_val_native_func("Iniciar", native_sqlite_begin));
    ng_table_set(connection, "Confirmar", ng_val_native_func("Confirmar", native_sqlite_commit));
    ng_table_set(connection, "Desfazer", ng_val_native_func("Desfazer", native_sqlite_rollback));
    ng_table_set(connection, "Fechar", ng_val_native_func("Fechar", native_sqlite_close));
    ng_table_set(connection, "UltimoErro", ng_val_native_func("UltimoErro", native_sqlite_last_error));
    ng_table_set(connection, "Params", ng_val_native_func("Params", native_sqlite_params));
    ng_table_set(connection, "Ajuda", ng_val_native_func("Ajuda", native_sqlite_help));
    ng_table_set(connection, "Versao", ng_val_native_func("Versao", native_sqlite_version));

    g_active_db = db;
    return ng_val_table(connection);
}

static NGValue native_sqlite_list_global(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGValue connection = db_table_for_active();
    NGTable* previous = interp->receiver_table;
    interp->receiver_table = connection.as.table;
    NGValue result = native_sqlite_params(interp, args, argc);
    interp->receiver_table = previous;
    ng_val_free(&connection);
    return result;
}

#else /* NG_NO_SQLITE */

static NGValue sqlite_unavailable(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    ng_error_print(NG_ERR_DB_MODULE, "SQLite Nao Disponivel", "<sqlite>", 0, 0,
                   "Esta compilacao da NG foi feita sem a biblioteca sqlite3.",
                   "Recompile com sqlite3.h e libsqlite3 instalados para habilitar @sqlite.");
    return ng_val_null();
}

static NGValue sqlite_unavailable_help(NGInterpreter* interp, NGValue* args, size_t argc) {
    NGTable* rows = ng_array_new();
    ng_array_push(rows, ng_val_string("@sqlite indisponivel nesta compilacao (sem libsqlite3)."));
    ng_array_push(rows, ng_val_string("Instale sqlite3 dev e recompile a NG."));
    (void)interp; (void)args; (void)argc;
    return ng_val_table(rows);
}

#endif /* NG_NO_SQLITE */

void ng_sqlite_module_load(NGInterpreter* interp) {
    if (!interp || !interp->globals) return;

    NGTable* module = ng_table_new(8);
#ifndef NG_NO_SQLITE
    ng_table_set(module, "__call", ng_val_native_func("__call", native_sqlite_open));
    ng_table_set(module, "novo", ng_val_native_func("novo", native_sqlite_open));
    ng_table_set(module, "Novo", ng_val_native_func("Novo", native_sqlite_open));
    ng_table_set(module, "Abrir", ng_val_native_func("Abrir", native_sqlite_open));
    ng_table_set(module, "Open", ng_val_native_func("Open", native_sqlite_open));
    ng_table_set(module, "Versao", ng_val_native_func("Versao", native_sqlite_version));
    ng_table_set(module, "Ajuda", ng_val_native_func("Ajuda", native_sqlite_help));
    ng_table_set(module, "Disponivel", ng_val_bool(true));
    ng_env_set(interp->globals, "list", ng_val_native_func("list", native_sqlite_list_global));
#else
    ng_table_set(module, "Abrir", ng_val_native_func("Abrir", sqlite_unavailable));
    ng_table_set(module, "Versao", ng_val_native_func("Versao", sqlite_unavailable));
    ng_table_set(module, "Ajuda", ng_val_native_func("Ajuda", sqlite_unavailable_help));
    ng_table_set(module, "Disponivel", ng_val_bool(false));
#endif

    ng_env_set(interp->globals, "SQLite", ng_val_table(module));
    /* Dois nomes apontam para a mesma tabela: cada binding possui uma
       referencia propria para evitar liberacao dupla. */
    ng_table_retain(module);
    ng_env_set(interp->globals, "SQLite3", ng_val_table(module));
}
