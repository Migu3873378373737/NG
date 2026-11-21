#ifndef NG_N3CACHE_H
#define NG_N3CACHE_H

#include "ng_ast.h"
#include <stdbool.h>
#include <stddef.h>

#define NG_VERSION "0.3.0-alpha"

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
