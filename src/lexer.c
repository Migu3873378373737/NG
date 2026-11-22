#include "ng_lexer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void ng_lexer_init(NGLexer* lexer, const char* source, const char* filename) {
    lexer->source = source;
    lexer->filename = filename;
    lexer->length = strlen(source);
    lexer->cursor = 0;
    lexer->line = 1;
    lexer->column = 1;
}

static char peek(NGLexer* l) {
    if (l->cursor >= l->length) return '\0';
    return l->source[l->cursor];
}

static char advance(NGLexer* l) {
    char c = peek(l);
    if (c != '\0') {
        l->cursor++;
        if (c == '\n') {
            l->line++;
            l->column = 1;
        } else {
            l->column++;
        }
    }
    return c;
}

static bool text_equals(const char* text, size_t len, const char* word) {
    size_t word_len = strlen(word);
    return len == word_len && memcmp(text, word, len) == 0;
}

static bool skip_whitespace_and_comments(NGLexer* l) {
    bool skipped = false;
    while (l->cursor < l->length) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
            skipped = true;
        } else if (c == '#' || (c == '/' && l->cursor + 1 < l->length && l->source[l->cursor + 1] == '/')) {
            // Comentário de linha única
            skipped = true;
            while (peek(l) != '\0' && peek(l) != '\n') {
                advance(l);
            }
        } else {
            break;
        }
    }
    return skipped;
}

void ng_token_free(NGToken* token) {
    if (token && token->text) {
        free(token->text);
        token->text = NULL;
    }
}

NGToken ng_lexer_next_token(NGLexer* l) {
    bool spaced = skip_whitespace_and_comments(l);

    NGToken tok;
    tok.text = NULL;
    tok.number_value = 0.0;
    tok.line = l->line;
    tok.column = l->column;
    tok.space_before = spaced;

    if (l->cursor >= l->length) {
        tok.type = NG_TOK_EOF;
        return tok;
    }

    char c = peek(l);

    // Delimitadores de Bloco e Pontuação: texto nao e necessario, o tipo
    // do token basta para o parser (evita uma alocacao por pontuacao).
    if (c == '[') { advance(l); tok.type = NG_TOK_LBRACKET; return tok; }
    if (c == ']') { advance(l); tok.type = NG_TOK_RBRACKET; return tok; }
    if (c == '(') { advance(l); tok.type = NG_TOK_LPAREN;   return tok; }
    if (c == ')') { advance(l); tok.type = NG_TOK_RPAREN;   return tok; }
    if (c == '@') { advance(l); tok.type = NG_TOK_AT;       return tok; }
    if (c == '.') { advance(l); tok.type = NG_TOK_DOT;      return tok; }
    if (c == ',') { advance(l); tok.type = NG_TOK_COMMA;    return tok; }
    if (c == '+') { advance(l); tok.type = NG_TOK_PLUS;     return tok; }
    if (c == '-') { advance(l); tok.type = NG_TOK_MINUS;    return tok; }
    if (c == '*') { advance(l); tok.type = NG_TOK_STAR;     return tok; }
    if (c == '/') { advance(l); tok.type = NG_TOK_SLASH;    return tok; }
    if (c == '%') { advance(l); tok.type = NG_TOK_PERCENT;  return tok; }

    // Operadores relacionais e de atribuição
    if (c == '=') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_EQ;
        } else {
            tok.type = NG_TOK_ASSIGN;
        }
        return tok;
    }
    if (c == '!') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_NEQ;
            return tok;
        }
        tok.type = NG_TOK_INVALID;
        return tok;
    }
    if (c == '<') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_LTE;
        } else {
            tok.type = NG_TOK_LT;
        }
        return tok;
    }
    if (c == '>') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_GTE;
        } else {
            tok.type = NG_TOK_GT;
        }
        return tok;
    }

    // Literais de String
    if (c == '"' || c == '\'') {
        char quote = advance(l);
        size_t cap = 32, len = 0;
        char* str = (char*)malloc(cap);
        while (peek(l) != '\0' && peek(l) != quote) {
            char ch = advance(l);
            if (ch == '\\' && peek(l) != '\0') {
                char e = advance(l);
                switch (e) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case '0': ch = '\0'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    case '\'': ch = '\''; break;
                    default:
                        if (len + 2 >= cap) { cap *= 2; str = (char*)realloc(str, cap); }
                        str[len++] = '\\';
                        ch = e;
                }
            }
            if (len + 2 >= cap) { cap *= 2; str = (char*)realloc(str, cap); }
            str[len++] = ch;
        }
        str[len] = '\0';
        if (peek(l) == quote) advance(l);
        tok.type = NG_TOK_STRING;
        tok.text = str;
        return tok;
    }

    // Literais Numéricos
    if (isdigit(c)) {
        size_t start = l->cursor;
        while (isdigit(peek(l))) advance(l);
        if (peek(l) == '.' && isdigit(l->source[l->cursor + 1])) {
            advance(l);
            while (isdigit(peek(l))) advance(l);
        }
        /* O parser usa apenas number_value. strtod para no primeiro caractere
           nao numerico, entao podemos converter direto do source: zero malloc. */
        tok.type = NG_TOK_NUMBER;
        tok.text = NULL;
        tok.number_value = strtod(l->source + start, NULL);
        return tok;
    }

    // Identificadores e Palavras Reservadas
    if (isalpha(c) || c == '_' || (unsigned char)c >= 0x80) {
        size_t start = l->cursor;
        while (isalnum(peek(l)) || peek(l) == '_' || (unsigned char)peek(l) >= 0x80) {
            advance(l);
        }
        size_t len = l->cursor - start;
        const char* text = l->source + start;

        NGTokenType type = NG_TOK_IDENTIFIER;
        if (text_equals(text, len, "Print")) type = NG_TOK_KW_PRINT;
        else if (text_equals(text, len, "G")) type = NG_TOK_KW_G;
        else if (text_equals(text, len, "Y")) type = NG_TOK_KW_Y;
        else if (text_equals(text, len, "Se")) type = NG_TOK_KW_SE;
        else if (text_equals(text, len, "Senao") || text_equals(text, len, "Senão")) type = NG_TOK_KW_SENAO;
        else if (text_equals(text, len, "Enquanto")) type = NG_TOK_KW_ENQUANTO;
        else if (text_equals(text, len, "Funcao") || text_equals(text, len, "Função") ||
                 text_equals(text, len, "funcao") || text_equals(text, len, "função")) type = NG_TOK_KW_FUNCAO;
        else if (text_equals(text, len, "Importar") || text_equals(text, len, "importar")) type = NG_TOK_KW_IMPORTAR;
        else if (text_equals(text, len, "como")) type = NG_TOK_KW_COMO;
        else if (text_equals(text, len, "true") || text_equals(text, len, "Verdadeiro")) type = NG_TOK_KW_TRUE;
        else if (text_equals(text, len, "false") || text_equals(text, len, "Falso")) type = NG_TOK_KW_FALSE;
        else if (text_equals(text, len, "null") || text_equals(text, len, "Nulo")) type = NG_TOK_KW_NULL;
        else if (text_equals(text, len, "fim") || text_equals(text, len, "Fim")) type = NG_TOK_KW_FIM;
        else if (text_equals(text, len, "Loop") || text_equals(text, len, "loop")) type = NG_TOK_KW_LOOP;
        else if (text_equals(text, len, "in") || text_equals(text, len, "In")) type = NG_TOK_KW_IN;

        tok.type = type;
        if (type == NG_TOK_IDENTIFIER) {
            char* id = (char*)malloc(len + 1);
            if (!id) {
                tok.type = NG_TOK_INVALID;
                return tok;
            }
            memcpy(id, text, len);
            id[len] = '\0';
            tok.text = id;
        }
        return tok;
    }

    // Token não reconhecido
    advance(l);
    tok.type = NG_TOK_INVALID;
    return tok;
}
