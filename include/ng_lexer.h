#ifndef NG_LEXER_H
#define NG_LEXER_H

#include <stddef.h>

typedef enum {
    NG_TOK_EOF = 0,
    NG_TOK_IDENTIFIER,
    NG_TOK_NUMBER,
    NG_TOK_STRING,
    
    // Palavras reservadas da NG
    NG_TOK_KW_PRINT,      // "Print"
    NG_TOK_KW_G,          // "G"
    NG_TOK_KW_Y,          // "Y"
    NG_TOK_KW_SE,         // "Se"
    NG_TOK_KW_SENAO,      // "Senao"
    NG_TOK_KW_ENQUANTO,   // "Enquanto"
    NG_TOK_KW_FUNCAO,     // "Função" ou "Funcao"
    NG_TOK_KW_IMPORTAR,   // "Importar"
    NG_TOK_KW_COMO,       // "como"
    NG_TOK_KW_TRUE,       // "true" ou "Verdadeiro"
    NG_TOK_KW_FALSE,      // "false" ou "Falso"
    NG_TOK_KW_NULL,       // "null" ou "Nulo"
    NG_TOK_KW_FIM,        // "fim" ou "Fim"
    NG_TOK_KW_LOOP,       // "Loop" ou "loop"
    NG_TOK_KW_IN,         // "in"
    
    // Delimitadores de blocos e pontuação
    NG_TOK_LBRACKET,      // '['
    NG_TOK_RBRACKET,      // ']'
    NG_TOK_LPAREN,        // '('
    NG_TOK_RPAREN,        // ')'
    NG_TOK_ASSIGN,        // '='
    NG_TOK_AT,            // '@'
    NG_TOK_DOT,           // '.'
    NG_TOK_COMMA,         // ','
    
    // Operadores
    NG_TOK_PLUS,          // '+'
    NG_TOK_MINUS,         // '-'
    NG_TOK_STAR,          // '*'
    NG_TOK_SLASH,         // '/'
    NG_TOK_PERCENT,       // '%'
    NG_TOK_EQ,            // '=='
    NG_TOK_NEQ,           // '!='
    NG_TOK_LT,            // '<'
    NG_TOK_LTE,           // '<='
    NG_TOK_GT,            // '>'
    NG_TOK_GTE,           // '>='
    NG_TOK_AND,           // '&&' ou 'e'
    NG_TOK_OR,            // '||' ou 'ou'
    NG_TOK_INVALID        // Caractere/token nao reconhecido
} NGTokenType;

typedef struct {
    NGTokenType type;
    char* text;
    double number_value;
    int line;
    int column;
} NGToken;

typedef struct {
    const char* source;
    const char* filename;
    size_t length;
    size_t cursor;
    int line;
    int column;
} NGLexer;

void ng_lexer_init(NGLexer* lexer, const char* source, const char* filename);
NGToken ng_lexer_next_token(NGLexer* lexer);
void ng_token_free(NGToken* token);

#endif // NG_LEXER_H
