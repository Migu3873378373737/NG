#include "ng_lexer.h"
#include "ng_parser.h"
#include "ng_interpreter.h"
#include "ng_error.h"
#include "ng_http.h"
#include "ng_n3cache.h"
#include "ng_repl.h"
#include "ng_dependency.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("  independencia install <pacote|URL> [-g]   Instala dependencia (alias: des i)\n");
    printf("  independencia desinstalar <pacote|URL>    Remove dependencia (alias: des des)\n");
    printf("  independencia list                        Lista dependencias (alias: des list)\n");
    printf("  versao                  Exibe versao do runtime NG\n");
    printf("  ajuda                   Exibe esta mensagem de ajuda\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        ng_native_modules_init();
        return ng_repl_run();
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

    if (strcmp(cmd, "executar") == 0 || strcmp(cmd, "verificar") == 0 || strcmp(cmd, "compilar") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Erro: Arquivo '.ng' nao especificado.\n");
            fprintf(stderr, "Uso: ng %s <arquivo.ng>\n", cmd);
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
            return 1;
        }

        NGAstNode* program = NULL;

        if (strcmp(cmd, "executar") == 0) {
            program = n3_cache_load_ast(filepath, source);
        }

        if (!program) {
            NGLexer lexer;
            ng_lexer_init(&lexer, source, filepath);

            NGParser parser;
            ng_parser_init(&parser, &lexer, filepath);

            program = ng_parser_parse_program(&parser);
            ng_parser_cleanup(&parser);

            if (parser.has_error) {
                ng_ast_free(program);
                free(source);
                return 1;
            }

            if (strcmp(cmd, "executar") == 0 || strcmp(cmd, "compilar") == 0) {
                n3_cache_store_ast(filepath, source, program);
            }
        }

        if (strcmp(cmd, "verificar") == 0) {
            printf("[NG] Sintaxe verificada com sucesso em '%s'. Nenhum erro encontrado.\n", filepath);
            ng_ast_free(program);
            free(source);
            return 0;
        }

        if (strcmp(cmd, "compilar") == 0) {
            printf("[NG/N3] AST compilada e cache salvo para '%s'.\n", filepath);
            ng_ast_free(program);
            free(source);
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
    return 1;
}
