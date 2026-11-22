#ifndef NG_N3CACHE_H
#define NG_N3CACHE_H

#include "ng_ast.h"
#include <stdbool.h>
#include <stddef.h>

#define NG_VERSION "0.3.0-alpha"

/* Cache N3: chave compacta (hash truncado), limite simples de entradas e
   formato binario com magic + versao + hash de origem + AST serializada. */
#define N3_CACHE_DIR ".n3cache"
#define N3_CACHE_KEY_HEX_LEN 12   /* chave curta: 12 hex do SHA-256 da origem */
#define N3_CACHE_MAX_ENTRIES 128  /* limite simples de entradas cacheadas */

/* Funções de hashing SHA-256 */
void ng_sha256(const unsigned char* data, size_t len, unsigned char hash[32]);
void ng_sha256_hex(const unsigned char* data, size_t len, char hex_out[65]);

/* Sistema de Cache N3: hashing e validação */
bool n3_cache_is_valid(const char* filepath);
bool n3_cache_store(const char* filepath, const char* compiled_blob, size_t size);
char* n3_cache_load(const char* filepath, size_t* out_size);
void n3_cache_clean(void);

/* Serialização e carregamento de AST no N3Cache */
bool n3_cache_store_ast(const char* filepath, const char* source, const NGAstNode* ast);
NGAstNode* n3_cache_load_ast(const char* filepath, const char* source);

#endif // NG_N3CACHE_H
