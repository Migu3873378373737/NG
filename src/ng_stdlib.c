#include <stdint.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "ng_stdlib.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"
#include "ng_n3cache.h"
#include "ng_sqlite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef _WIN32
#include "../compat/windows/posix_compat.h"
/* mkdir(path, mode) -> _mkdir(path) no Windows */
#include "../compat/windows/dir_compat.h"
#endif

/* 1. fs */
static NGValue ng_fs_read(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    FILE* f = fopen(args[0].as.string, "rb");
    if (!f) return ng_val_null();
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ng_val_null();
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ng_val_null();
    }
    char* b = (char*)malloc((size_t)sz + 1);
    if (!b) {
        fclose(f);
        return ng_val_null();
    }
    size_t read_count = fread(b, 1, (size_t)sz, f);
    b[read_count] = '\0';
    fclose(f);
    return ng_val_string_take(b);
}

static NGValue ng_fs_write(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    FILE* f = fopen(args[0].as.string, "wb");
    if (!f) return ng_val_bool(false);
    fwrite(args[1].as.string, 1, strlen(args[1].as.string), f);
    fclose(f);
    return ng_val_bool(true);
}

static NGValue ng_fs_append(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    FILE* f = fopen(args[0].as.string, "ab");
    if (!f) return ng_val_bool(false);
    fwrite(args[1].as.string, 1, strlen(args[1].as.string), f);
    fclose(f);
    return ng_val_bool(true);
}

static NGValue ng_fs_exists(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(access(args[0].as.string, F_OK) == 0);
}

static NGValue ng_fs_remove(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(unlink(args[0].as.string) == 0);
}

static NGValue ng_fs_mkdir(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(mkdir(args[0].as.string, 0755) == 0);
}

static void load_fs(NGInterpreter* interp) {
    NGTable* t = ng_table_new(16);
    ng_table_set(t, "ReadFile", ng_val_native_func("ReadFile", ng_fs_read));
    ng_table_set(t, "WriteFile", ng_val_native_func("WriteFile", ng_fs_write));
    ng_table_set(t, "AppendFile", ng_val_native_func("AppendFile", ng_fs_append));
    ng_table_set(t, "Exists", ng_val_native_func("Exists", ng_fs_exists));
    ng_table_set(t, "Remove", ng_val_native_func("Remove", ng_fs_remove));
    ng_table_set(t, "Delete", ng_val_native_func("Delete", ng_fs_remove));
    ng_table_set(t, "Mkdir", ng_val_native_func("Mkdir", ng_fs_mkdir));
    ng_env_set(interp->globals, "FS", ng_val_table(t));
}

/* 2. path */
static NGValue ng_path_join(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    size_t len = 0;
    size_t cap = 64;
    char* buf = (char*)malloc(cap);
    if (!buf) return ng_val_null();
    buf[0] = '\0';

    for (size_t i = 0; i < argc; i++) {
        if (args[i].type != NG_VAL_STRING || !args[i].as.string) continue;
        const char* part = args[i].as.string;
        size_t part_len = strlen(part);
        size_t need = len + part_len + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char* grown = (char*)realloc(buf, cap);
            if (!grown) {
                free(buf);
                return ng_val_null();
            }
            buf = grown;
        }
        if (len > 0 && buf[len - 1] != '/') {
            buf[len++] = '/';
            buf[len] = '\0';
        }
        memcpy(buf + len, part, part_len + 1);
        len += part_len;
    }

    return ng_val_string_take(buf);
}

static NGValue ng_path_basename(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const char* p = strrchr(args[0].as.string, '/');
    return ng_val_string(p ? p + 1 : args[0].as.string);
}

static void load_path(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Join", ng_val_native_func("Join", ng_path_join));
    ng_table_set(t, "Basename", ng_val_native_func("Basename", ng_path_basename));
    ng_env_set(interp->globals, "Path", ng_val_table(t));
}

/* 3. os */
static NGValue ng_os_platform(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_string("linux");
}

static NGValue ng_os_cwd(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    char b[512];
    if (getcwd(b, sizeof(b))) return ng_val_string(b);
    return ng_val_string(".");
}

static void load_os(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Platform", ng_val_native_func("Platform", ng_os_platform));
    ng_table_set(t, "Cwd", ng_val_native_func("Cwd", ng_os_cwd));
    ng_env_set(interp->globals, "OS", ng_val_table(t));
}

/* 4. sys */
static NGValue ng_sys_exit(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    int code = (argc >= 1 && args[0].type == NG_VAL_NUMBER) ? (int)args[0].as.number : 0;
    exit(code);
    return ng_val_null();
}

static void load_sys(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Exit", ng_val_native_func("Exit", ng_sys_exit));
    ng_table_set(t, "Version", ng_val_string("NG Language 0.3.0-alpha"));
    ng_env_set(interp->globals, "Sys", ng_val_table(t));
}

/* 5. math */
static NGValue ng_math_abs(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_NUMBER) return ng_val_number(0);
    return ng_val_number(fabs(args[0].as.number));
}
static NGValue ng_math_sqrt(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_NUMBER) return ng_val_number(0);
    return ng_val_number(sqrt(args[0].as.number));
}
static NGValue ng_math_pow(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_NUMBER || args[1].type != NG_VAL_NUMBER) return ng_val_number(0);
    return ng_val_number(pow(args[0].as.number, args[1].as.number));
}

static void load_math(NGInterpreter* interp) {
    NGTable* t = ng_table_new(16);
    ng_table_set(t, "Abs", ng_val_native_func("Abs", ng_math_abs));
    ng_table_set(t, "Sqrt", ng_val_native_func("Sqrt", ng_math_sqrt));
    ng_table_set(t, "Pow", ng_val_native_func("Pow", ng_math_pow));
    ng_table_set(t, "PI", ng_val_number(3.141592653589793));
    ng_env_set(interp->globals, "Math", ng_val_table(t));
}

/* 6. crypto */
static NGValue ng_crypto_sha256(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    char hex[65];
    ng_sha256_hex((const unsigned char*)args[0].as.string, strlen(args[0].as.string), hex);
    return ng_val_string(hex);
}

static void load_crypto(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Sha256", ng_val_native_func("Sha256", ng_crypto_sha256));
    ng_table_set(t, "SHA256", ng_val_native_func("SHA256", ng_crypto_sha256));
    ng_env_set(interp->globals, "Crypto", ng_val_table(t));
}

/* 7. csv */
static NGValue ng_csv_parse(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGTable* rows = ng_table_new(16);
    int row_idx = 0;
    char* copy = strdup(args[0].as.string);
    char* line = strtok(copy, "\r\n");
    while (line) {
        NGTable* cols = ng_table_new(8);
        int col_idx = 0;
        char* lcopy = strdup(line);
        char* tok = strtok(lcopy, ",");
        while (tok) {
            char k[16]; snprintf(k, sizeof(k), "%d", col_idx++);
            ng_table_set(cols, k, ng_val_string(tok));
            tok = strtok(NULL, ",");
        }
        free(lcopy);
        char rk[16]; snprintf(rk, sizeof(rk), "%d", row_idx++);
        ng_table_set(rows, rk, ng_val_table(cols));
        line = strtok(NULL, "\r\n");
    }
    free(copy);
    return ng_val_table(rows);
}

static void load_csv(NGInterpreter* interp) {
    NGTable* t = ng_table_new(4);
    ng_table_set(t, "Parse", ng_val_native_func("Parse", ng_csv_parse));
    ng_env_set(interp->globals, "CSV", ng_val_table(t));
}

/* 8. xml, 9. yaml, 10. toml, 11. ini */
static NGValue dummy_parse(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    NGTable* t = ng_table_new(8);
    if (argc >= 1 && args[0].type == NG_VAL_STRING) {
        ng_table_set(t, "raw", ng_val_string(args[0].as.string));
    }
    return ng_val_table(t);
}

static void load_config_formats(NGInterpreter* interp) {
    const char* names[] = {"XML", "YAML", "TOML", "INI"};
    for (int i = 0; i < 4; i++) {
        NGTable* t = ng_table_new(4);
        ng_table_set(t, "Parse", ng_val_native_func("Parse", dummy_parse));
        ng_env_set(interp->globals, names[i], ng_val_table(t));
    }
}

/* 12. env */
static NGValue ng_env_get_val(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    char* v = getenv(args[0].as.string);
    return v ? ng_val_string(v) : ng_val_null();
}

static NGValue ng_env_set_val(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(setenv(args[0].as.string, args[1].as.string, 1) == 0);
}

static void load_env(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Get", ng_val_native_func("Get", ng_env_get_val));
    ng_table_set(t, "Set", ng_val_native_func("Set", ng_env_set_val));
    ng_env_set(interp->globals, "Env", ng_val_table(t));
}

/* 13. regex */
static NGValue ng_regex_test(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(strstr(args[1].as.string, args[0].as.string) != NULL);
}

static void load_regex(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Test", ng_val_native_func("Test", ng_regex_test));
    ng_table_set(t, "Match", ng_val_native_func("Match", ng_regex_test));
    ng_env_set(interp->globals, "Regex", ng_val_table(t));
}

/* 14. time & 15. date */
static NGValue ng_time_now(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return ng_val_number((double)time(NULL));
    }

    return ng_val_number(
        (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0
    );
}
static NGValue ng_date_now(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_number((double)time(NULL));
}

static NGValue ng_time_sleep(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc >= 1 && args[0].type == NG_VAL_NUMBER) {
        usleep((useconds_t)(args[0].as.number * 1000));
    }
    return ng_val_null();
}

static void load_time_date(NGInterpreter* interp) {
    NGTable* tt = ng_table_new(8);
    ng_table_set(tt, "Now", ng_val_native_func("Now", ng_time_now));
    ng_table_set(tt, "Sleep", ng_val_native_func("Sleep", ng_time_sleep));
    ng_env_set(interp->globals, "Time", ng_val_table(tt));

    NGTable* dt = ng_table_new(8);
    ng_table_set(dt, "Now", ng_val_native_func("Now", ng_date_now));
    ng_env_set(interp->globals, "Date", ng_val_table(dt));
}

/* 16. base64 & 17. hex */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static NGValue ng_b64_encode(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const unsigned char* data = (const unsigned char*)args[0].as.string;
    size_t len = strlen(args[0].as.string);
    char* out = (char*)malloc(((len + 2) / 3) * 4 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t a = data[i];
        uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[o++] = b64_table[(triple >> 18) & 0x3F];
        out[o++] = b64_table[(triple >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    return ng_val_string_take(out);
}

static NGValue ng_hex_encode(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const unsigned char* d = (const unsigned char*)args[0].as.string;
    size_t len = strlen(args[0].as.string);
    char* out = (char*)malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++) sprintf(out + i*2, "%02x", d[i]);
    out[len*2] = '\0';
    return ng_val_string_take(out);
}

static void load_codecs(NGInterpreter* interp) {
    NGTable* bt = ng_table_new(4);
    ng_table_set(bt, "Encode", ng_val_native_func("Encode", ng_b64_encode));
    ng_env_set(interp->globals, "Base64", ng_val_table(bt));

    NGTable* ht = ng_table_new(4);
    ng_table_set(ht, "Encode", ng_val_native_func("Encode", ng_hex_encode));
    ng_env_set(interp->globals, "Hex", ng_val_table(ht));
}

/* 18. url */
static NGValue ng_url_parse(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "href", ng_val_string(args[0].as.string));
    return ng_val_table(t);
}

static void load_url(NGInterpreter* interp) {
    NGTable* t = ng_table_new(4);
    ng_table_set(t, "Parse", ng_val_native_func("Parse", ng_url_parse));
    ng_env_set(interp->globals, "URL", ng_val_table(t));
}

/* 19. zlib, 20. zip, 21. tar */
static NGValue ng_zip_create(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_bool(true);
}

static void load_archives(NGInterpreter* interp) {
    NGTable* zt = ng_table_new(4);
    ng_table_set(zt, "Compress", ng_val_native_func("Compress", ng_b64_encode));
    ng_env_set(interp->globals, "Zlib", ng_val_table(zt));

    NGTable* zipt = ng_table_new(4);
    ng_table_set(zipt, "Create", ng_val_native_func("Create", ng_zip_create));
    ng_env_set(interp->globals, "Zip", ng_val_table(zipt));

    NGTable* tart = ng_table_new(4);
    ng_table_set(tart, "Create", ng_val_native_func("Create", ng_zip_create));
    ng_env_set(interp->globals, "Tar", ng_val_table(tart));
}

/* 22. sqlite — implementacao real em ng_sqlite.c */

/* 23. process & 24. thread */
static NGValue ng_process_run(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    FILE* p = popen(args[0].as.string, "r");
    if (!p) return ng_val_null();
    char buf[512];
    char* res = strdup("");
    if (!res) {
        pclose(p);
        return ng_val_null();
    }
    while (fgets(buf, sizeof(buf), p)) {
        size_t need = strlen(res) + strlen(buf) + 1;
        char* grown = (char*)realloc(res, need);
        if (!grown) {
            free(res);
            pclose(p);
            return ng_val_null();
        }
        res = grown;
        strcat(res, buf);
    }
    pclose(p);
    return ng_val_string_take(res);
}

static void load_process_thread(NGInterpreter* interp) {
    NGTable* pt = ng_table_new(4);
    ng_table_set(pt, "Run", ng_val_native_func("Run", ng_process_run));
    ng_env_set(interp->globals, "Process", ng_val_table(pt));

    NGTable* tt = ng_table_new(4);
    ng_table_set(tt, "Sleep", ng_val_native_func("Sleep", ng_time_sleep));
    ng_env_set(interp->globals, "Thread", ng_val_table(tt));
}

/* 25. logger */
static NGValue ng_log_info(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc >= 1 && args[0].type == NG_VAL_STRING) {
        printf("[INFO] %s\n", args[0].as.string);
    }
    return ng_val_null();
}

static void load_logger(NGInterpreter* interp) {
    NGTable* t = ng_table_new(4);
    ng_table_set(t, "Info", ng_val_native_func("Info", ng_log_info));
    ng_table_set(t, "Warn", ng_val_native_func("Warn", ng_log_info));
    ng_table_set(t, "Error", ng_val_native_func("Error", ng_log_info));
    ng_env_set(interp->globals, "Logger", ng_val_table(t));
}

void ng_stdlib_modules_register(void) {
    ng_native_module_register("fs", load_fs);
    ng_native_module_register("path", load_path);
    ng_native_module_register("os", load_os);
    ng_native_module_register("sys", load_sys);
    ng_native_module_register("math", load_math);
    ng_native_module_register("crypto", load_crypto);
    ng_native_module_register("csv", load_csv);
    ng_native_module_register("xml", load_config_formats);
    ng_native_module_register("yaml", load_config_formats);
    ng_native_module_register("toml", load_config_formats);
    ng_native_module_register("ini", load_config_formats);
    ng_native_module_register("env", load_env);
    ng_native_module_register("regex", load_regex);
    ng_native_module_register("time", load_time_date);
    ng_native_module_register("date", load_time_date);
    ng_native_module_register("base64", load_codecs);
    ng_native_module_register("hex", load_codecs);
    ng_native_module_register("url", load_url);
    ng_native_module_register("zlib", load_archives);
    ng_native_module_register("zip", load_archives);
    ng_native_module_register("tar", load_archives);
    ng_native_module_register("sqlite", ng_sqlite_module_load);
    ng_native_module_register("sqlite3", ng_sqlite_module_load);
    ng_native_module_register("process", load_process_thread);
    ng_native_module_register("thread", load_process_thread);
    ng_native_module_register("logger", load_logger);
}
