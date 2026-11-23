#ifndef NG_PARSER_H
#define NG_PARSER_H

#include "ng_lexer.h"
#include "ng_ast.h"

typedef struct {
    NGLexer* lexer;
    NGToken current;
    NGToken previous;
    bool has_error;
    bool disable_index; /* evita que '[' de bloco seja lido como indice */
    const char* filename;
} NGParser;

void ng_parser_init(NGParser* parser, NGLexer* lexer, const char* filename);
void ng_parser_cleanup(NGParser* parser);
NGAstNode* ng_parser_parse_program(NGParser* parser);

#endif // NG_PARSER_H
