#include "ng_repl.h"
#include "ng_lexer.h"
#include "ng_parser.h"
#include "ng_interpreter.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPL_READ_CHUNK 1024
#define REPL_INITIAL_AST_CAPACITY 16

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} ReplBuffer;

typedef struct {
    int brackets;
    int parens;
} ReplDepth;

static bool repl_buf_append(ReplBuffer* buf, const char* text, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t new_cap = buf->cap > 0 ? buf->cap : 128;
        while (new_cap < buf->len + len + 1) {
            new_cap *= 2;
        }
        char* resized = (char*)realloc(buf->data, new_cap);
        if (!resized) return false;
        buf->data = resized;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

static void repl_buf_reset(ReplBuffer* buf) {
    buf->len = 0;
    if (buf->data) buf->data[0] = '\0';
}

static void repl_buf_free(ReplBuffer* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/*
 * Le uma linha inteira de stdin usando fgets, mesmo que ela seja maior
 * que o buffer interno. Retorna NULL em EOF (ou falha de memoria).
 */
static char* repl_read_line(void) {
    ReplBuffer line = {0};
    char chunk[REPL_READ_CHUNK];
    bool read_any = false;

    while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
        read_any = true;
        size_t len = strlen(chunk);
        if (!repl_buf_append(&line, chunk, len)) {
            repl_buf_free(&line);
            return NULL;
        }
        if (len > 0 && chunk[len - 1] == '\n') break;
    }

    if (!read_any) {
        repl_buf_free(&line);
        return NULL;
    }
    return line.data;
}

static bool repl_is_blank_line(const char* text) {
    while (*text != '\0') {
        if (!isspace((unsigned char)*text)) return false;
        text++;
    }
    return true;
}

static bool repl_ieq_n(const char* text, size_t len, const char* word) {
    size_t word_len = strlen(word);
    if (len != word_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)word[i])) {
            return false;
        }
    }
    return true;
}

static bool repl_is_exit_command(const char* text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    const char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    size_t len = (size_t)(end - text);
    return repl_ieq_n(text, len, "sair") || repl_ieq_n(text, len, "exit");
}

/*
 * Atualiza a profundidade de blocos abertos ('[' e '(') da linha.
 * Ignora delimitadores dentro de strings e comentarios para nao continuar
 * a leitura por engano em casos como: Print "[".
 */
static void repl_scan_depth(const char* text, ReplDepth* depth) {
    bool in_string = false;
    char quote = '\0';

    for (size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];

        if (in_string) {
            if (c == '\\' && text[i + 1] != '\0') {
                i++;
                continue;
            }
            if (c == quote) in_string = false;
            continue;
        }

        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            continue;
        }

        if (c == '#') break;
        if (c == '/' && text[i + 1] == '/') break;

        if (c == '[') {
            depth->brackets++;
        } else if (c == ']') {
            if (depth->brackets > 0) depth->brackets--;
        } else if (c == '(') {
            depth->parens++;
        } else if (c == ')') {
            if (depth->parens > 0) depth->parens--;
        }
    }
}

/*
 * Os valores de funcao guardam um ponteiro para o corpo na AST. Por isso,
 * programas executados no REPL precisam continuar vivos enquanto a sessao
 * durar; caso contrario, funcoes definidas em linhas anteriores teriam
 * corpos invalidos. Todos os programas sao liberados ao sair do REPL.
 */
static bool repl_retain_program(NGAstNode*** programs, size_t* count,
                                size_t* capacity, NGAstNode* program) {
    if (*count == *capacity) {
        size_t new_capacity = *capacity > 0 ? *capacity * 2 : REPL_INITIAL_AST_CAPACITY;
        NGAstNode** resized = (NGAstNode**)realloc(*programs, new_capacity * sizeof(NGAstNode*));
        if (!resized) return false;
        *programs = resized;
        *capacity = new_capacity;
    }
    (*programs)[(*count)++] = program;
    return true;
}

int ng_repl_run(void) {
    printf("NG %s\n", NG_REPL_VERSION);
    printf("REPL interativo. Digite 'sair' ou Ctrl+D para encerrar.\n");
    fflush(stdout);

    NGInterpreter interp;
    ng_interp_init(&interp, "<repl>", false);

    NGAstNode** retained_programs = NULL;
    size_t retained_count = 0;
    size_t retained_capacity = 0;

    ReplBuffer input = {0};
    ReplDepth depth = {0};
    int status = 0;

    while (true) {
        printf("%s", (depth.brackets > 0 || depth.parens > 0) ? ">> " : "> ");
        fflush(stdout);

        char* line = repl_read_line();
        if (!line) {
            if (depth.brackets > 0 || depth.parens > 0) {
                fprintf(stderr, "\n[NG/REPL] Entrada incompleta: bloco nao fechado.\n");
            }
            printf("\n");
            fflush(stdout);
            break;
        }

        /* Comandos de saida e linhas vazias so valem fora de um bloco. */
        if (depth.brackets == 0 && depth.parens == 0) {
            if (repl_is_blank_line(line)) {
                free(line);
                continue;
            }
            if (repl_is_exit_command(line)) {
                free(line);
                break;
            }
        }

        if (!repl_buf_append(&input, line, strlen(line))) {
            fprintf(stderr, "[NG/REPL] Erro: memoria insuficiente.\n");
            free(line);
            status = 1;
            break;
        }

        repl_scan_depth(line, &depth);
        free(line);

        if (depth.brackets > 0 || depth.parens > 0) {
            continue;
        }

        fflush(stdout);

        NGLexer lexer;
        ng_lexer_init(&lexer, input.data, "<repl>");

        NGParser parser;
        ng_parser_init(&parser, &lexer, "<repl>");

        NGAstNode* program = ng_parser_parse_program(&parser);
        ng_parser_cleanup(&parser);

        if (parser.has_error) {
            /* O parser ja reportou o erro; apenas descartamos esta entrada. */
            ng_ast_free(program);
        } else if (program && program->as.program.count > 0) {
            if (repl_retain_program(&retained_programs, &retained_count,
                                    &retained_capacity, program)) {
                ng_interp_run(&interp, program);
            } else {
                ng_ast_free(program);
                fprintf(stderr, "[NG/REPL] Erro: memoria insuficiente ao manter o programa.\n");
                status = 1;
            }
        } else {
            ng_ast_free(program);
        }

        fflush(stdout);
        repl_buf_reset(&input);
        depth.brackets = 0;
        depth.parens = 0;
    }

    ng_interp_cleanup(&interp);
    for (size_t i = 0; i < retained_count; i++) {
        ng_ast_free(retained_programs[i]);
    }
    free(retained_programs);
    repl_buf_free(&input);

    return status;
}
