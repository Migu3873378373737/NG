#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_interop.h"
#include "ng_process.h"
#include "ng_json.h"
#include "ng_native_module.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* interop_extension(const char* lang) {
    if (strcmp(lang, "python") == 0 || strcmp(lang, "py") == 0) return "py";
    if (strcmp(lang, "lua") == 0) return "lua";
    if (strcmp(lang, "go") == 0 || strcmp(lang, "golang") == 0) return "go";
    return NULL;
}

static const char* interop_temp_dir(void) {
    const char* d = getenv("TMPDIR");
    if (d && *d) return d;
    d = getenv("PREFIX");
    if (d && *d) {
        static char path[512];
        snprintf(path, sizeof(path), "%s/tmp", d);
        struct stat st;
        if (stat(path, &st) == 0) return path;
    }
    return "/tmp";
}

static char* interop_write_temp(const char* code, const char* ext) {
    char templ[768];
    snprintf(templ, sizeof(templ), "%s/ng_interop_XXXXXX.%s", interop_temp_dir(), ext);
    int fd = mkstemps(templ, (int)strlen(ext) + 1);
    if (fd < 0) return NULL;

    FILE* f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(templ);
        return NULL;
    }
    fputs(code, f);
    fclose(f);
    return strdup(templ);
}

static char* interop_trim_copy(const char* text) {
    if (!text) return strdup("");
    while (*text && isspace((unsigned char)*text)) text++;
    const char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    size_t len = (size_t)(end - text);
    char* out = (char*)malloc(len + 1);
    if (!out) return strdup("");
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

/* Executa codigo nativo da linguagem alvo e devolve a saida (malloc). */
static char* interop_run(const char* lang, const char* code, const char* input_json) {
    const char* ext = interop_extension(lang);
    if (!ext) {
        char* msg = (char*)malloc(128);
        snprintf(msg, 128, "[Interop] Linguagem nao suportada: %s", lang);
        return msg;
    }

    char* path = interop_write_temp(code, ext);
    if (!path) return strdup("[Interop] Falha ao criar arquivo temporario.");

    NGProcessResult result;
    const char* command = NULL;

    if (strcmp(ext, "py") == 0) {
        char* argv[] = {(char*)"python3", (char*)path, NULL};
        command = "python3";
        result = ng_process_run(argv, input_json);
        if (result.exit_code == 127) {
            ng_process_result_free(&result);
            char* argv2[] = {(char*)"python", (char*)path, NULL};
            command = "python";
            result = ng_process_run(argv2, input_json);
        }
    } else if (strcmp(ext, "lua") == 0) {
        char* argv[] = {(char*)"lua", (char*)path, NULL};
        command = "lua";
        result = ng_process_run(argv, input_json);
    } else {
        char* argv[] = {(char*)"go", (char*)"run", (char*)path, NULL};
        command = "go";
        result = ng_process_run(argv, input_json);
    }

    unlink(path);
    free(path);

    if (result.exit_code == 127) {
        char* msg = (char*)malloc(160);
        snprintf(msg, 160, "[Interop] '%s' nao encontrado no PATH.", command);
        ng_process_result_free(&result);
        return msg;
    }

    char* out = interop_trim_copy(result.output ? result.output : "");
    ng_process_result_free(&result);
    return out;
}

static NGValue native_interop_exec(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) {
        return ng_val_null();
    }
    char* out = interop_run(args[0].as.string, args[1].as.string, NULL);
    return ng_val_string_take(out);
}

static NGValue native_interop_lang(NGInterpreter* interp, NGValue* args, size_t argc,
                                   const char* lang) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    char* out = interop_run(lang, args[0].as.string, NULL);
    return ng_val_string_take(out);
}

static NGValue native_interop_python(NGInterpreter* i, NGValue* a, size_t n) {
    return native_interop_lang(i, a, n, "python");
}
static NGValue native_interop_lua(NGInterpreter* i, NGValue* a, size_t n) {
    return native_interop_lang(i, a, n, "lua");
}
static NGValue native_interop_go(NGInterpreter* i, NGValue* a, size_t n) {
    return native_interop_lang(i, a, n, "go");
}

/* Envia um valor NG como JSON via NG_INPUT e converte a saida JSON de volta. */
static NGValue native_interop_exchange(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 3 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) {
        return ng_val_null();
    }
    char* input = ng_json_stringify(&args[2]);
    char* out = interop_run(args[0].as.string, args[1].as.string, input ? input : "null");
    free(input);
    if (!out || *out == '\0') {
        free(out);
        return ng_val_null();
    }
    NGValue result = ng_json_parse(out);
    free(out);
    return result;
}

static NGValue native_interop_to_json(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1) return ng_val_string("null");
    char* json = ng_json_stringify(&args[0]);
    if (!json) return ng_val_string("null");
    return ng_val_string_take(json);
}

static NGValue native_interop_from_json(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    return ng_json_parse(args[0].as.string);
}

void ng_interop_module_load(NGInterpreter* interp) {
    if (!interp || !interp->globals) return;

    NGTable* t = ng_table_new(16);
    ng_table_set(t, "Executar", ng_val_native_func("Executar", native_interop_exec));
    ng_table_set(t, "Run", ng_val_native_func("Run", native_interop_exec));
    ng_table_set(t, "Python", ng_val_native_func("Python", native_interop_python));
    ng_table_set(t, "Lua", ng_val_native_func("Lua", native_interop_lua));
    ng_table_set(t, "Go", ng_val_native_func("Go", native_interop_go));
    ng_table_set(t, "Golang", ng_val_native_func("Golang", native_interop_go));
    ng_table_set(t, "Trocar", ng_val_native_func("Trocar", native_interop_exchange));
    ng_table_set(t, "Exchange", ng_val_native_func("Exchange", native_interop_exchange));
    ng_table_set(t, "ParaJSON", ng_val_native_func("ParaJSON", native_interop_to_json));
    ng_table_set(t, "ParaNG", ng_val_native_func("ParaNG", native_interop_from_json));

    ng_env_set(interp->globals, "Interop", ng_val_table(t));
}

void ng_interop_module_register(void) {
    ng_native_module_register("interop", ng_interop_module_load);
    ng_native_module_register("interoperabilidade", ng_interop_module_load);
}
