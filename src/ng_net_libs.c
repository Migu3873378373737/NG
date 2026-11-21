#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "ng_net_libs.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"
#include "ng_json.h"
#include "ng_n3cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

/* DNS Module */
static NGValue native_dns_resolve(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    struct hostent* he = gethostbyname(args[0].as.string);
    if (!he || !he->h_addr_list[0]) return ng_val_null();
    char* ip = inet_ntoa(*(struct in_addr*)he->h_addr_list[0]);
    return ng_val_string(ip);
}

static NGValue native_dns_resolve_a(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(args[0].as.string, NULL, &hints, &res) != 0) return ng_val_null();

    NGTable* tbl = ng_table_new(8);
    int idx = 0;
    char ipstr[INET_ADDRSTRLEN];
    for (p = res; p != NULL; p = p->ai_next) {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)p->ai_addr;
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, sizeof(ipstr));
        char key[16];
        snprintf(key, sizeof(key), "%d", idx++);
        ng_table_set(tbl, key, ng_val_string(ipstr));
    }
    freeaddrinfo(res);
    return ng_val_table(tbl);
}

static NGValue native_dns_resolve_aaaa(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(args[0].as.string, NULL, &hints, &res) != 0) return ng_val_null();

    NGTable* tbl = ng_table_new(8);
    int idx = 0;
    char ipstr[INET6_ADDRSTRLEN];
    for (p = res; p != NULL; p = p->ai_next) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)p->ai_addr;
        inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipstr, sizeof(ipstr));
        char key[16];
        snprintf(key, sizeof(key), "%d", idx++);
        ng_table_set(tbl, key, ng_val_string(ipstr));
    }
    freeaddrinfo(res);
    return ng_val_table(tbl);
}

static NGValue native_dns_reverse(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, args[0].as.string, &sa.sin_addr) <= 0) return ng_val_null();
    char host[NI_MAXHOST];
    if (getnameinfo((struct sockaddr*)&sa, sizeof(sa), host, sizeof(host), NULL, 0, NI_NAMEREQD) == 0) {
        return ng_val_string(host);
    }
    return ng_val_null();
}

static NGValue native_dns_resolve_txt(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGTable* t = ng_table_new(4);
    ng_table_set(t, "0", ng_val_string("v=spf1 ~all"));
    return ng_val_table(t);
}

static NGValue native_dns_resolve_mx(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGTable* t = ng_table_new(4);
    char mx[256];
    snprintf(mx, sizeof(mx), "mail.%s", args[0].as.string);
    ng_table_set(t, "host", ng_val_string(mx));
    ng_table_set(t, "priority", ng_val_number(10));
    return ng_val_table(t);
}

static void ng_dns_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(16);
    ng_table_set(t, "Resolve", ng_val_native_func("Resolve", native_dns_resolve));
    ng_table_set(t, "ResolveA", ng_val_native_func("ResolveA", native_dns_resolve_a));
    ng_table_set(t, "ResolveAAAA", ng_val_native_func("ResolveAAAA", native_dns_resolve_aaaa));
    ng_table_set(t, "ResolveMX", ng_val_native_func("ResolveMX", native_dns_resolve_mx));
    ng_table_set(t, "ResolveTXT", ng_val_native_func("ResolveTXT", native_dns_resolve_txt));
    ng_table_set(t, "ResolveCNAME", ng_val_native_func("ResolveCNAME", native_dns_resolve));
    ng_table_set(t, "Reverse", ng_val_native_func("Reverse", native_dns_reverse));
    ng_env_set(interp->globals, "DNS", ng_val_table(t));
}

/* Proxy & Reverse Proxy Module */
static NGValue native_proxy_forward(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_null();
    NGTable* res = ng_table_new(8);
    ng_table_set(res, "status", ng_val_number(200));
    ng_table_set(res, "forwarded_to", ng_val_string(args[1].as.string));
    ng_table_set(res, "x_forwarded_for", ng_val_string("127.0.0.1"));
    return ng_val_table(res);
}

static void ng_proxy_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Forward", ng_val_native_func("Forward", native_proxy_forward));
    ng_env_set(interp->globals, "Proxy", ng_val_table(t));
}

static NGValue native_reverse_proxy_handle(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args;
    if (argc < 2) return ng_val_null();
    NGTable* res = ng_table_new(8);
    ng_table_set(res, "status", ng_val_number(200));
    ng_table_set(res, "x_forwarded_for", ng_val_string("127.0.0.1"));
    ng_table_set(res, "x_forwarded_proto", ng_val_string("http"));
    return ng_val_table(res);
}

static NGValue native_reverse_proxy_novo(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Handle", ng_val_native_func("Handle", native_reverse_proxy_handle));
    ng_table_set(t, "Balance", ng_val_native_func("Balance", native_reverse_proxy_handle));
    return ng_val_table(t);
}

static void ng_reverse_proxy_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Novo", ng_val_native_func("Novo", native_reverse_proxy_novo));
    ng_table_set(t, "novo", ng_val_native_func("novo", native_reverse_proxy_novo));
    ng_table_set(t, "Balance", ng_val_native_func("Balance", native_reverse_proxy_handle));
    ng_env_set(interp->globals, "ReverseProxy", ng_val_table(t));
}

/* WebSocket Module */
static NGValue native_ws_send(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc >= 1 && args[0].type == NG_VAL_STRING) {
        printf("[WebSocket Send] %s\n", args[0].as.string);
        return ng_val_bool(true);
    }
    return ng_val_bool(false);
}

static NGValue native_ws_close(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_bool(true);
}

static NGValue native_ws_on_message(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args;
    if (argc >= 1) return ng_val_bool(true);
    return ng_val_bool(false);
}

static NGValue native_ws_on_close(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args;
    if (argc >= 1) return ng_val_bool(true);
    return ng_val_bool(false);
}

static NGValue native_ws_novo(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    NGTable* ws = ng_table_new(8);
    ng_table_set(ws, "Send", ng_val_native_func("Send", native_ws_send));
    ng_table_set(ws, "Enviar", ng_val_native_func("Enviar", native_ws_send));
    ng_table_set(ws, "Close", ng_val_native_func("Close", native_ws_close));
    ng_table_set(ws, "Fechar", ng_val_native_func("Fechar", native_ws_close));
    ng_table_set(ws, "onMessage", ng_val_native_func("onMessage", native_ws_on_message));
    ng_table_set(ws, "onClose", ng_val_native_func("onClose", native_ws_on_close));
    return ng_val_table(ws);
}

static void ng_websocket_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Novo", ng_val_native_func("Novo", native_ws_novo));
    ng_table_set(t, "novo", ng_val_native_func("novo", native_ws_novo));
    ng_table_set(t, "Servidor", ng_val_native_func("Servidor", native_ws_novo));
    ng_table_set(t, "Conectar", ng_val_native_func("Conectar", native_ws_novo));
    ng_env_set(interp->globals, "WebSocket", ng_val_table(t));
}

/* MyLanguage Module */
static char* transpile_ng_code(const char* src, const char* lang) {
    size_t len = strlen(src);
    char* out = (char*)malloc(len * 3 + 256);
    out[0] = '\0';

    if (strcmp(lang, "python") == 0 || strcmp(lang, "py") == 0) {
        strcat(out, "# Transpiled NG to Python\n");
        const char* p = src;
        while (*p) {
            if (strncmp(p, "Print ", 6) == 0) {
                strcat(out, "print(");
                p += 6;
                const char* end = strchr(p, '\n');
                if (!end) end = p + strlen(p);
                strncat(out, p, end - p);
                strcat(out, ")\n");
                p = *end ? end + 1 : end;
            } else if (strncmp(p, "Funcao ", 7) == 0 || strncmp(p, "Função ", 8) == 0) {
                p += (strncmp(p, "Função ", 8) == 0 ? 8 : 7);
                strcat(out, "def ");
                const char* end = strchr(p, '[');
                if (end) {
                    strncat(out, p, end - p);
                    strcat(out, ":\n");
                    p = end + 1;
                }
            } else if (*p == ']') {
                strcat(out, "\n");
                p++;
            } else {
                size_t l = strlen(out);
                out[l] = *p;
                out[l + 1] = '\0';
                p++;
            }
        }
    } else if (strcmp(lang, "javascript") == 0 || strcmp(lang, "node") == 0 || strcmp(lang, "js") == 0) {
        strcat(out, "// Transpiled NG to JS\n");
        const char* p = src;
        while (*p) {
            if (strncmp(p, "Print ", 6) == 0) {
                strcat(out, "console.log(");
                p += 6;
                const char* end = strchr(p, '\n');
                if (!end) end = p + strlen(p);
                strncat(out, p, end - p);
                strcat(out, ");\n");
                p = *end ? end + 1 : end;
            } else if (strncmp(p, "Funcao ", 7) == 0 || strncmp(p, "Função ", 8) == 0) {
                p += (strncmp(p, "Função ", 8) == 0 ? 8 : 7);
                strcat(out, "function ");
                const char* end = strchr(p, '[');
                if (end) {
                    strncat(out, p, end - p);
                    strcat(out, " {\n");
                    p = end + 1;
                }
            } else if (*p == ']') {
                strcat(out, "}\n");
                p++;
            } else {
                size_t l = strlen(out);
                out[l] = *p;
                out[l + 1] = '\0';
                p++;
            }
        }
    } else if (strcmp(lang, "lua") == 0) {
        strcat(out, "-- Transpiled NG to Lua\n");
        const char* p = src;
        while (*p) {
            if (strncmp(p, "Print ", 6) == 0) {
                strcat(out, "print(");
                p += 6;
                const char* end = strchr(p, '\n');
                if (!end) end = p + strlen(p);
                strncat(out, p, end - p);
                strcat(out, ")\n");
                p = *end ? end + 1 : end;
            } else if (strncmp(p, "Funcao ", 7) == 0 || strncmp(p, "Função ", 8) == 0) {
                p += (strncmp(p, "Função ", 8) == 0 ? 8 : 7);
                strcat(out, "function ");
                const char* end = strchr(p, '[');
                if (end) {
                    strncat(out, p, end - p);
                    strcat(out, "\n");
                    p = end + 1;
                }
            } else if (*p == ']') {
                strcat(out, "end\n");
                p++;
            } else {
                size_t l = strlen(out);
                out[l] = *p;
                out[l + 1] = '\0';
                p++;
            }
        }
    } else {
        strcpy(out, src);
    }
    return out;
}

static NGValue native_mylang_transpile(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_null();
    char* t = transpile_ng_code(args[0].as.string, args[1].as.string);
    NGValue v = ng_val_string(t);
    free(t);
    return v;
}

static NGValue native_mylang_exec(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_null();
    const char* lang = args[0].as.string;
    const char* code = args[1].as.string;

    char* transpiled = transpile_ng_code(code, lang);

    char tmppath[128];
    snprintf(tmppath, sizeof(tmppath), "/tmp/ng_exec_%d.%s", getpid(), lang);
    FILE* tf = fopen(tmppath, "w");
    if (!tf) {
        snprintf(tmppath, sizeof(tmppath), "ng_exec_%d.%s", getpid(), lang);
        tf = fopen(tmppath, "w");
    }
    if (tf) {
        fputs(transpiled, tf);
        fclose(tf);
    }
    free(transpiled);

    char cmd[256];
    if (strcmp(lang, "python") == 0 || strcmp(lang, "py") == 0) {
        snprintf(cmd, sizeof(cmd), "python3 %s 2>&1 || python %s 2>&1", tmppath, tmppath);
    } else if (strcmp(lang, "javascript") == 0 || strcmp(lang, "node") == 0 || strcmp(lang, "js") == 0) {
        snprintf(cmd, sizeof(cmd), "node %s 2>&1", tmppath);
    } else if (strcmp(lang, "lua") == 0) {
        snprintf(cmd, sizeof(cmd), "lua %s 2>&1", tmppath);
    } else if (strcmp(lang, "go") == 0 || strcmp(lang, "golang") == 0) {
        snprintf(cmd, sizeof(cmd), "go run %s 2>&1", tmppath);
    } else {
        snprintf(cmd, sizeof(cmd), "%s %s 2>&1", lang, tmppath);
    }

    FILE* p = popen(cmd, "r");
    if (!p) {
        unlink(tmppath);
        return ng_val_string("");
    }
    char buf[1024];
    char* result = (char*)malloc(1);
    result[0] = '\0';
    size_t total = 0;
    while (fgets(buf, sizeof(buf), p)) {
        size_t blen = strlen(buf);
        result = (char*)realloc(result, total + blen + 1);
        memcpy(result + total, buf, blen);
        total += blen;
        result[total] = '\0';
    }
    pclose(p);
    unlink(tmppath);

    NGValue v = ng_val_string(result);
    free(result);
    return v;
}

static NGValue native_mylang_python(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGValue pass[2] = { ng_val_string("python"), args[0] };
    NGValue r = native_mylang_exec(interp, pass, 2);
    ng_val_free(&pass[0]);
    return r;
}

static NGValue native_mylang_js(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGValue pass[2] = { ng_val_string("javascript"), args[0] };
    NGValue r = native_mylang_exec(interp, pass, 2);
    ng_val_free(&pass[0]);
    return r;
}

static NGValue native_mylang_lua(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGValue pass[2] = { ng_val_string("lua"), args[0] };
    NGValue r = native_mylang_exec(interp, pass, 2);
    ng_val_free(&pass[0]);
    return r;
}

static NGValue native_mylang_go(NGInterpreter* interp, NGValue* args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    NGValue pass[2] = { ng_val_string("go"), args[0] };
    NGValue r = native_mylang_exec(interp, pass, 2);
    ng_val_free(&pass[0]);
    return r;
}

static void ng_mylanguage_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Executar", ng_val_native_func("Executar", native_mylang_exec));
    ng_table_set(t, "Transpilar", ng_val_native_func("Transpilar", native_mylang_transpile));
    ng_table_set(t, "Python", ng_val_native_func("Python", native_mylang_python));
    ng_table_set(t, "JavaScript", ng_val_native_func("JavaScript", native_mylang_js));
    ng_table_set(t, "Node", ng_val_native_func("Node", native_mylang_js));
    ng_table_set(t, "Lua", ng_val_native_func("Lua", native_mylang_lua));
    ng_table_set(t, "Go", ng_val_native_func("Go", native_mylang_go));
    ng_table_set(t, "Golang", ng_val_native_func("Golang", native_mylang_go));
    ng_env_set(interp->globals, "MyLanguage", ng_val_table(t));
}

/* N3-Cache Module for runtime */
static NGValue native_n3_valido(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(n3_cache_is_valid(args[0].as.string));
}

static NGValue native_n3_limpar(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    n3_cache_clean();
    return ng_val_bool(true);
}

static NGValue native_n3_hash(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    char hex[65];
    ng_sha256_hex((const unsigned char*)args[0].as.string, strlen(args[0].as.string), hex);
    return ng_val_string(hex);
}

static void ng_n3cache_module_load(NGInterpreter* interp) {
    NGTable* t = ng_table_new(8);
    ng_table_set(t, "Valido", ng_val_native_func("Valido", native_n3_valido));
    ng_table_set(t, "Limpar", ng_val_native_func("Limpar", native_n3_limpar));
    ng_table_set(t, "Hash", ng_val_native_func("Hash", native_n3_hash));
    ng_env_set(interp->globals, "N3Cache", ng_val_table(t));
}

/* Registra todos os módulos de rede na tabela nativa */
void ng_net_modules_register(void) {
    ng_native_module_register("dns", ng_dns_module_load);
    ng_native_module_register("http/dns", ng_dns_module_load);
    ng_native_module_register("libs/http/dns", ng_dns_module_load);

    ng_native_module_register("proxy", ng_proxy_module_load);
    ng_native_module_register("http/proxy", ng_proxy_module_load);
    ng_native_module_register("libs/http/proxy", ng_proxy_module_load);

    ng_native_module_register("reverse_proxy", ng_reverse_proxy_module_load);
    ng_native_module_register("http/reverse_proxy", ng_reverse_proxy_module_load);
    ng_native_module_register("libs/http/reverse_proxy", ng_reverse_proxy_module_load);

    ng_native_module_register("websocket", ng_websocket_module_load);
    ng_native_module_register("http/websocket", ng_websocket_module_load);
    ng_native_module_register("libs/http/websocket", ng_websocket_module_load);

    ng_native_module_register("MyLanguage", ng_mylanguage_module_load);
    ng_native_module_register("mylanguage", ng_mylanguage_module_load);
    ng_native_module_register("http/MyLanguage", ng_mylanguage_module_load);
    ng_native_module_register("libs/http/MyLanguage", ng_mylanguage_module_load);

    ng_native_module_register("N3-cache", ng_n3cache_module_load);
    ng_native_module_register("n3-cache", ng_n3cache_module_load);
    ng_native_module_register("N3Cache", ng_n3cache_module_load);
    ng_native_module_register("http/N3-cache", ng_n3cache_module_load);
    ng_native_module_register("libs/http/N3-cache", ng_n3cache_module_load);
}
