#include "ng_native_module.h"
#include "ng_stdlib.h"
#include "ng_net_libs.h"
#include "ng_http.h"
#include "ng_json.h"
#include "ng_interop.h"
#include "ng_print.h"
#include "ng_qr.h"
#include "ng_cripto.h"
#include "ng_ws.h"
#include "ng_zap.h"
#include "ng_wacore.h"
#include <string.h>
#include <stdlib.h>

#define NG_MAX_NATIVE_MODULES 128

typedef struct {
    char name[128];
    NGNativeModuleLoader loader;
} NGNativeModuleEntry;

static NGNativeModuleEntry g_modules[NG_MAX_NATIVE_MODULES];
static size_t g_module_count = 0;

void ng_native_module_register(const char* name, NGNativeModuleLoader loader) {
    if (!name || !loader) return;
    /* Update if already registered */
    for (size_t i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, name) == 0) {
            g_modules[i].loader = loader;
            return;
        }
    }
    if (g_module_count >= NG_MAX_NATIVE_MODULES) return;
    strncpy(g_modules[g_module_count].name, name, sizeof(g_modules[g_module_count].name) - 1);
    g_modules[g_module_count].name[sizeof(g_modules[g_module_count].name) - 1] = '\0';
    g_modules[g_module_count].loader = loader;
    g_module_count++;
}

bool ng_native_module_load(const char* name, NGInterpreter* interp) {
    if (!name || !interp) return false;
    const char* clean_name = name;
    if (clean_name[0] == '@') clean_name++;
    for (size_t i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i].name, clean_name) == 0 || strcmp(g_modules[i].name, name) == 0) {
            if (g_modules[i].loader) {
                g_modules[i].loader(interp);
            }
            return true;
        }
    }
    return false;
}

NGValue ng_native_module_invoke(NGInterpreter* interp, const char* module_name, const char* func_name, NGValue* args, size_t argc) {
    if (!interp || !module_name || !func_name) return ng_val_null();
    if (!ng_native_module_load(module_name, interp)) return ng_val_null();
    NGValue module_val;
    if (!ng_env_get(interp->globals, module_name, &module_val) || module_val.type != NG_VAL_TABLE) return ng_val_null();
    NGValue fn;
    if (!ng_table_get(module_val.as.table, func_name, &fn) || fn.type != NG_VAL_NATIVE_FUNC) return ng_val_null();
    return ng_interp_call_func(interp, &fn, args, argc);
}

void ng_native_modules_init(void) {
    ng_native_module_register("http", ng_http_module_load);
    ng_native_module_register("https", ng_http_module_load);
    ng_native_module_register("http/https", ng_http_module_load);
    ng_native_module_register("libs/http/https", ng_http_module_load);
    ng_native_module_register("json", ng_json_module_load);
    ng_native_module_register("http/json", ng_json_module_load);
    ng_native_module_register("libs/http/json", ng_json_module_load);

    ng_stdlib_modules_register();
    ng_net_modules_register();
    ng_interop_module_register();
    ng_print_modules_register();
    ng_qr_module_register();
    ng_cripto_module_register();
    ng_ws_module_register();
    ng_zap_module_register();
    ng_wacore_module_register();
}
