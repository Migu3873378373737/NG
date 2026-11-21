#include "ng_native_module.h"
#include "ng_stdlib.h"
#include "ng_net_libs.h"
#include "ng_http.h"
#include "ng_json.h"
#include "ng_interop.h"
#include "ng_print.h"
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
}
