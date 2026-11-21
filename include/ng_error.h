#ifndef NG_ERROR_H
#define NG_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Códigos de Erro Oficiais da NG
#define NG_ERR_UNKNOWN       "NG-001"
#define NG_ERR_FILE_NOT_FOUND "NG-002"
#define NG_ERR_PERMISSION    "NG-003"
#define NG_ERR_INVALID_TOKEN "NG-101"
#define NG_ERR_UNCLOSED_BLOCK "NG-103"
#define NG_ERR_UNEXPECTED_BLOCK "NG-104"
#define NG_ERR_SYNTAX        "NG-106"
#define NG_ERR_UNDEFINED_VAR "NG-107"
#define NG_ERR_TYPE_MISMATCH "NG-108"
#define NG_ERR_UNDEFINED_FUNC "NG-109"
#define NG_ERR_INVALID_ARGS  "NG-110"
#define NG_ERR_CACHE_INVALID "NG-201"
#define NG_ERR_MODULE_NOT_FOUND "NG-301"
#define NG_ERR_MODULE_CONFLICT "NG-504"
#define NG_ERR_SUBINTERP_FAIL "NG-501"
#define NG_ERR_SQL            "NG-401"
#define NG_ERR_SQL_CONNECT    "NG-402"
#define NG_ERR_SQL_PARAM      "NG-403"
#define NG_ERR_SQL_EMPTY      "NG-404"
#define NG_ERR_DB_MODULE      "NG-405"
#define NG_ERR_JSON           "NG-406"
#define NG_ERR_DIAG           "NG-407"

static inline void ng_error_print(const char* code, const char* title, const char* file, int line, int col, const char* desc, const char* suggestion) {
    fprintf(stderr, "\n[%s] %s\n\n", code, title);
    fprintf(stderr, "Arquivo: %s\n", file ? file : "entrada");
    fprintf(stderr, "Linha: %d\nColuna: %d\n\n", line, col);
    fprintf(stderr, "Descrição:\n%s\n\n", desc);
    if (suggestion && strlen(suggestion) > 0) {
        fprintf(stderr, "Sugestão:\n%s\n\n", suggestion);
    }
}

#endif // NG_ERROR_H
