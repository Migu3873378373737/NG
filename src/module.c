#include "ng_module.h"
#include "ng_native_module.h"
#include "ng_lexer.h"
#include "ng_parser.h"
#include "ng_interpreter.h"
#include "ng_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_module_source(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char* b = (char*)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t read_count = fread(b, 1, (size_t)sz, f);
    b[read_count] = '\0';
    fclose(f);
    return b;
}

bool ng_module_load_into_env(const char* module_name, NGInterpreter* interp) {
    /* Try native module first */
    if (ng_native_module_load(module_name, interp)) {
        return true;
    }

    const char* mname = module_name;
    if (mname[0] == '@') mname++;

    /* Try .ng file in libs and NG-modulos */
    char path[512];
    snprintf(path, sizeof(path), "libs/%s/index.ng", mname);
    char* src = read_module_source(path);
    if (!src) {
        snprintf(path, sizeof(path), "NG-modulos/%s/index.ng", mname);
        src = read_module_source(path);
    }
    if (!src) {
        snprintf(path, sizeof(path), "libs/%s.ng", mname);
        src = read_module_source(path);
    }

    /* Modulos instalados globalmente pelo Independência (-g). */
    if (!src) {
        const char* prefix = getenv("PREFIX");
        if (prefix && *prefix) {
            snprintf(path, sizeof(path), "%.400s/lib/ng/modulos/%s/index.ng", prefix, mname);
            src = read_module_source(path);
        }
    }
    if (!src) {
        const char* prefix = getenv("PREFIX");
        if (prefix && *prefix) {
            snprintf(path, sizeof(path), "%.400s/lib/ng/modulos/%s.ng", prefix, mname);
            src = read_module_source(path);
        }
    }

    if (!src) {
        ng_error_print(NG_ERR_MODULE_NOT_FOUND, "Modulo Nao Encontrado", module_name, 0, 0,
                       "O modulo solicitado nao foi encontrado.",
                       "Verifique se o modulo esta instalado em 'NG-modulos/<nome>/index.ng' ou registrado como modulo nativo.");
        return false;
    }

    NGLexer lex;
    ng_lexer_init(&lex, src, path);
    NGParser par;
    ng_parser_init(&par, &lex, path);
    NGAstNode* mod_ast = ng_parser_parse_program(&par);
    ng_parser_cleanup(&par);

    if (!par.has_error && mod_ast) {
        ng_interp_run(interp, mod_ast);
    }

    /* IMPORTANTE: NAO liberar mod_ast nem src. As funcoes definidas no modulo guardam
       ponteiros para o corpo na AST; liberar aqui deixava toda funcao de modulo
       "vazia" (retornava null sem executar nada). O custo e so um pouco de memoria. */
    (void)mod_ast;
    return !par.has_error;
}
