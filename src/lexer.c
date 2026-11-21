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

static void skip_whitespace_and_comments(NGLexer* l) {
    while (l->cursor < l->length) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
        } else if (c == '#' || (c == '/' && l->cursor + 1 < l->length && l->source[l->cursor + 1] == '/')) {
            // Comentário de linha única
            while (peek(l) != '\0' && peek(l) != '\n') {
                advance(l);
            }
        } else {
            break;
        }
    }
}

void ng_token_free(NGToken* token) {
    if (token && token->text) {
        free(token->text);
        token->text = NULL;
    }
}

NGToken ng_lexer_next_token(NGLexer* l) {
    skip_whitespace_and_comments(l);

    NGToken tok;
    tok.text = NULL;
    tok.number_value = 0.0;
    tok.line = l->line;
    tok.column = l->column;

    if (l->cursor >= l->length) {
        tok.type = NG_TOK_EOF;
        return tok;
    }

    char c = peek(l);

    // Delimitadores de Bloco e Pontuação
    if (c == '[') { advance(l); tok.type = NG_TOK_LBRACKET; tok.text = strdup("["); return tok; }
    if (c == ']') { advance(l); tok.type = NG_TOK_RBRACKET; tok.text = strdup("]"); return tok; }
    if (c == '(') { advance(l); tok.type = NG_TOK_LPAREN;   tok.text = strdup("("); return tok; }
    if (c == ')') { advance(l); tok.type = NG_TOK_RPAREN;   tok.text = strdup(")"); return tok; }
    if (c == '@') { advance(l); tok.type = NG_TOK_AT;       tok.text = strdup("@"); return tok; }
    if (c == '.') { advance(l); tok.type = NG_TOK_DOT;      tok.text = strdup("."); return tok; }
    if (c == ',') { advance(l); tok.type = NG_TOK_COMMA;    tok.text = strdup(","); return tok; }
    if (c == '+') { advance(l); tok.type = NG_TOK_PLUS;     tok.text = strdup("+"); return tok; }
    if (c == '-') { advance(l); tok.type = NG_TOK_MINUS;    tok.text = strdup("-"); return tok; }
    if (c == '*') { advance(l); tok.type = NG_TOK_STAR;     tok.text = strdup("*"); return tok; }
    if (c == '/') { advance(l); tok.type = NG_TOK_SLASH;    tok.text = strdup("/"); return tok; }
    if (c == '%') { advance(l); tok.type = NG_TOK_PERCENT;  tok.text = strdup("%"); return tok; }

    // Operadores relacionais e de atribuição
    if (c == '=') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_EQ;
            tok.text = strdup("==");
        } else {
            tok.type = NG_TOK_ASSIGN;
            tok.text = strdup("=");
        }
        return tok;
    }
    if (c == '!') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_NEQ;
            tok.text = strdup("!=");
            return tok;
        }
        tok.type = NG_TOK_INVALID;
        tok.text = strdup("!");
        return tok;
    }
    if (c == '<') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_LTE;
            tok.text = strdup("<=");
        } else {
            tok.type = NG_TOK_LT;
            tok.text = strdup("<");
        }
        return tok;
    }
    if (c == '>') {
        advance(l);
        if (peek(l) == '=') {
            advance(l);
            tok.type = NG_TOK_GTE;
            tok.text = strdup(">=");
        } else {
            tok.type = NG_TOK_GT;
            tok.text = strdup(">");
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
        char* id = (char*)malloc(len + 1);
        memcpy(id, l->source + start, len);
        id[len] = '\0';

        tok.text = id;
        if (strcmp(id, "Print") == 0) tok.type = NG_TOK_KW_PRINT;
        else if (strcmp(id, "G") == 0) tok.type = NG_TOK_KW_G;
        else if (strcmp(id, "Y") == 0) tok.type = NG_TOK_KW_Y;
        else if (strcmp(id, "Se") == 0) tok.type = NG_TOK_KW_SE;
        else if (strcmp(id, "Senao") == 0 || strcmp(id, "Senão") == 0) tok.type = NG_TOK_KW_SENAO;
        else if (strcmp(id, "Enquanto") == 0) tok.type = NG_TOK_KW_ENQUANTO;
        else if (strcmp(id, "Funcao") == 0 || strcmp(id, "Função") == 0 ||
                 strcmp(id, "funcao") == 0 || strcmp(id, "função") == 0) tok.type = NG_TOK_KW_FUNCAO;
        else if (strcmp(id, "Importar") == 0 || strcmp(id, "importar") == 0) tok.type = NG_TOK_KW_IMPORTAR;
        else if (strcmp(id, "como") == 0) tok.type = NG_TOK_KW_COMO;
        else if (strcmp(id, "true") == 0 || strcmp(id, "Verdadeiro") == 0) tok.type = NG_TOK_KW_TRUE;
        else if (strcmp(id, "false") == 0 || strcmp(id, "Falso") == 0) tok.type = NG_TOK_KW_FALSE;
        else if (strcmp(id, "null") == 0 || strcmp(id, "Nulo") == 0) tok.type = NG_TOK_KW_NULL;
        else if (strcmp(id, "fim") == 0 || strcmp(id, "Fim") == 0) tok.type = NG_TOK_KW_FIM;
        else if (strcmp(id, "Loop") == 0 || strcmp(id, "loop") == 0) tok.type = NG_TOK_KW_LOOP;
        else if (strcmp(id, "in") == 0 || strcmp(id, "In") == 0) tok.type = NG_TOK_KW_IN;
        else tok.type = NG_TOK_IDENTIFIER;

        return tok;
    }

    // Token não reconhecido
    advance(l);
    tok.type = NG_TOK_INVALID;
    tok.text = strdup("?");
    return tok;
}
