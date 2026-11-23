#include "ng_lexer.h"
#include "ng_parser.h"
#include "ng_interpreter.h"
#include "ng_error.h"
#include "ng_http.h"
#include "ng_n3cache.h"
#include "ng_repl.h"
#include "ng_dependency.h"
#include "ng_net_libs.h"
#include "ng_dynamicv.h"
#include "ng_outbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Dynamic V Dual-Core (Invicta):
 *   - Core 0 (C11): executor / runtime nativo e fallback.
 *   - Core 1 (Lua embedded): worker de otimização semântica e IR.
 *   - L2 Barramento: fila de tarefas e sincronização C11.
 *   - N3 Cache: cache persistente de V-units em disco com escrita atômica.
 * ========================================================= */
static NGAstNode* v_prepare_program(const char* filepath, const char* source,
                                   bool store_compiled, bool* out_error) {
    return ng_dynamicv_prepare_program(filepath, source, store_compiled, out_error);
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    size_t read_count = fread(buffer, 1, (size_t)size, f);
    buffer[read_count] = '\0';
    fclose(f);
    return buffer;
}

static void print_help(void) {
    printf("NG — Next Generation Language (v0.3.0-alpha)\n");
    printf("Uso: ng [comando] [argumentos]\n\n");
    printf("Sem argumentos, inicia o REPL interativo (digite 'sair' ou Ctrl+D para encerrar).\n\n");
    printf("Comandos principais:\n");
    printf("  executar <arquivo.ng>   Executa um programa NG\n");
    printf("    --subinterpreter      Ativa subinterpretador isolado\n");
    printf("    --no-subinterpreter   Desativa subinterpretador (padrao)\n");
    printf("  compilar <arquivo.ng>   Valida sintaxe e prepara cache N3\n");
    printf("  verificar <arquivo.ng>  Verifica sintaxe e integridade\n");
    printf("  limpar-cache            Limpa o cache N3 do projeto\n");
    printf("  dynamicv status         Status do Dynamic V Dual-Core (Core 0/1, L2, estatísticas)\n");
    printf("  dynamicv cache          Inspeção do cache N3\n");
    printf("  dynamicv limpar         Limpa o cache N3 do Dynamic V\n");
    printf("  dynamicv diagnostico    Diagnóstico de ambiente + benchmark miss/hit\n");
    printf("  independencia install <pacote|URL> [-g]   Instala dependencia (alias: des i)\n");
    printf("  independencia desinstalar <pacote|URL>    Remove dependencia (alias: des des)\n");
    printf("  independencia list                        Lista dependencias (alias: des list)\n");
    printf("  versao                  Exibe versao do runtime NG\n");
    printf("  ajuda                   Exibe esta mensagem de ajuda\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        ng_native_modules_init();
        int rc = ng_repl_run();
        ng_outbuf_flush();
        return rc;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "versao") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("NG Language Engine v0.3.0-alpha (C11 Native Runtime)\n");
        printf("Plataforma: Linux/Android (Termux compatible)\n");
        return 0;
    }

    if (strcmp(cmd, "ajuda") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help();
        return 0;
    }

    if (strcmp(cmd, "limpar-cache") == 0) {
        n3_cache_clean();
        printf("[NG/N3] Cache limpo com sucesso.\n");
        return 0;
    }

    if (strcmp(cmd, "dynamicv") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Uso: ng dynamicv <status|cache|limpar|diagnostico>\n");
            return 1;
        }
        if (strcmp(argv[2], "status") == 0) { ng_dynamicv_status(stdout); return 0; }
        if (strcmp(argv[2], "cache") == 0) { ng_dynamicv_cache_info(stdout); return 0; }
        if (strcmp(argv[2], "limpar") == 0) { ng_dynamicv_clean_cache(); return 0; }
        if (strcmp(argv[2], "diagnostico") == 0) { ng_dynamicv_diagnostico(stdout); return 0; }
        fprintf(stderr, "Subcomando desconhecido: '%s'.\n", argv[2]);
        return 1;
    }

    if (strcmp(cmd, "executar") == 0 || strcmp(cmd, "verificar") == 0 || strcmp(cmd, "compilar") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Erro: Arquivo '.ng' nao especificado.\n");
            fprintf(stderr, "Uso: ng %s <arquivo.ng>\n", cmd);
            ng_outbuf_flush();
            return 1;
        }

        const char* filepath = argv[2];
        bool subinterpreter = false;

        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--subinterpreter") == 0) {
                subinterpreter = true;
            } else if (strcmp(argv[i], "--no-subinterpreter") == 0) {
                subinterpreter = false;
            }
        }

        char* source = read_file(filepath);
        if (!source) {
            ng_error_print(NG_ERR_FILE_NOT_FOUND, "Arquivo Nao Encontrado", filepath, 0, 0,
                           "Nao foi possivel abrir o arquivo especificado.", "Verifique o caminho do arquivo '.ng'.");
            ng_outbuf_flush();
            return 1;
        }

        /* Backend de execucao: "c" (padrao, fluxo Dynamic V/N3 Cache) ou "lua"
           (runtime externo via transpilacao). */
        const char* backend = "c";
        for (int i = 3; i < argc; i++) {
            if (strncmp(argv[i], "--backend=", 10) == 0) {
                backend = argv[i] + 10;
            }
        }
        const char* backend_env = getenv("NG_BACKEND");
        if (backend_env && backend_env[0]) backend = backend_env;

        /* Backend Lua: bypass do interpretador AST em C11 (transpila -> runtime Lua). */
        if (backend && strcmp(backend, "lua") == 0 && strcmp(cmd, "executar") == 0) {
            NGValue lua_out = ng_net_libs_exec_source(source, "lua", true);
            const char* out_str = (lua_out.type == NG_VAL_STRING) ? lua_out.as.string : NULL;
            if (out_str) fputs(out_str, stdout);
            ng_val_free(&lua_out);
            free(source);
            return 0;
        }

        /* Dynamic V: divisao FASE PREPARACAO (N3 HIT/MISS) -> FASE EXECUCAO. */
        bool v_error = false;
        NGAstNode* program = v_prepare_program(filepath, source,
                                                (strcmp(cmd, "executar") == 0 || strcmp(cmd, "compilar") == 0),
                                                &v_error);

        if (v_error) {
            free(source);
            ng_outbuf_flush();
            return 1;
        }

        if (strcmp(cmd, "verificar") == 0) {
            printf("[NG] Sintaxe verificada com sucesso em '%s'. Nenhum erro encontrado.\n", filepath);
            ng_ast_free(program);
            free(source);
            ng_outbuf_flush();
            return 0;
        }

        if (strcmp(cmd, "compilar") == 0) {
            printf("[NG/N3] AST compilada e cache salvo para '%s'.\n", filepath);
            ng_ast_free(program);
            free(source);
            ng_outbuf_flush();
            return 0;
        }

        // Executar
        ng_native_modules_init();
        NGInterpreter interp;
        ng_interp_init(&interp, filepath, subinterpreter);
        bool success = ng_interp_run(&interp, program);

        ng_interp_cleanup(&interp);
        ng_ast_free(program);
        free(source);
        ng_outbuf_flush();
        return success ? 0 : 1;
    }

    if (strcmp(cmd, "independência") == 0 || strcmp(cmd, "independencia") == 0 ||
        strcmp(cmd, "des") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[Independência] Subcomando ausente. Digite 'ng ajuda'.\n");
            return 1;
        }
        return ng_dependency_command(argc - 2, argv + 2);
    }

    fprintf(stderr, "Comando desconhecido: '%s'. Digite 'ng ajuda' para opcoes disponiveis.\n", cmd);
    ng_outbuf_flush();
    return 1;
}
