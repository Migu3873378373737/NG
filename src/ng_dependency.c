#define _POSIX_C_SOURCE 200809L
#include "ng_dependency.h"
#include "ng_process.h"
#ifndef NG_NO_CURL
#include "ng_http_client.h"
#endif
#ifdef _WIN32
/* mkdir(path, mode) -> _mkdir(path) no Windows (lstat nao existe la) */
#include "../compat/windows/dir_compat.h"
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEP_REGISTRY_FILE "Independência.txt.ng"
#define DEP_LOCAL_ROOT    "NG-modulos"
#define DEP_MAX_PACKAGES  512
#define DEP_PATH_MAX      1024
#define DEP_DEFAULT_REGISTRY \
    "https://raw.githubusercontent.com/ng-language/independencia/main/pacotes"

typedef struct {
    char nome[128];
    char origem[32];    /* nome | git | url */
    char url[512];
    char versao[128];
    char escopo[16];    /* local | global */
    char status[16];    /* instalado | removido */
    char caminho[DEP_PATH_MAX];
} Dependency;

typedef struct {
    Dependency itens[DEP_MAX_PACKAGES];
    size_t count;
} DependencyRegistry;

typedef enum {
    DEP_ORIGIN_NAME,
    DEP_ORIGIN_GIT,
    DEP_ORIGIN_URL
} DepOrigin;

/* ------------------------------------------------------------------ */
/* Utilitarios de filesystem                                           */
/* ------------------------------------------------------------------ */

static void dep_trim(char* text) {
    if (!text) return;
    while (*text && isspace((unsigned char)*text)) text++;
    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
}

static bool dep_exists(const char* path) {
    return path && path[0] && access(path, F_OK) == 0;
}

static bool dep_is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool dep_mkdir_p(const char* path) {
    if (!path || !*path) return false;
    char tmp[DEP_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' && mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                tmp[i] = saved;
                return false;
            }
            tmp[i] = saved;
        }
    }
    return true;
}

static void dep_remove_path(const char* path) {
    if (!path || !*path) return;
    struct stat st;
#ifdef _WIN32
    if (stat(path, &st) != 0) return;   /* sem symlinks no fluxo Windows */
#else
    if (lstat(path, &st) != 0) return;
#endif

    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char child[DEP_PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                dep_remove_path(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static bool dep_copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
}

static bool dep_copy_dir(const char* src, const char* dst) {
    if (!dep_mkdir_p(dst)) return false;
    DIR* d = opendir(src);
    if (!d) return false;
    struct dirent* e;
    bool ok = true;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[DEP_PATH_MAX], t[DEP_PATH_MAX];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        if (dep_is_dir(s)) {
            if (!dep_copy_dir(s, t)) ok = false;
        } else if (!dep_copy_file(s, t)) {
            ok = false;
        }
    }
    closedir(d);
    return ok;
}

static void dep_global_root(char* out, size_t cap) {
    const char* prefix = getenv("PREFIX");
    if (!prefix || !*prefix) prefix = "/data/data/com.termux/files/usr";
    snprintf(out, cap, "%s/lib/ng/modulos", prefix);
}

static void dep_root_for(const char* scope, char* out, size_t cap) {
    if (strcmp(scope, "global") == 0) {
        dep_global_root(out, cap);
    } else {
        snprintf(out, cap, "%s", DEP_LOCAL_ROOT);
    }
}

/* ------------------------------------------------------------------ */
/* Registro Independência.txt.ng                                       */
/* ------------------------------------------------------------------ */

static void dep_registry_assign(Dependency* d, const char* key, const char* value) {
    if (strcmp(key, "nome") == 0 || strcmp(key, "name") == 0) {
        snprintf(d->nome, sizeof(d->nome), "%s", value);
    } else if (strcmp(key, "origem") == 0 || strcmp(key, "origin") == 0) {
        snprintf(d->origem, sizeof(d->origem), "%s", value);
    } else if (strcmp(key, "url") == 0) {
        snprintf(d->url, sizeof(d->url), "%s", value);
    } else if (strcmp(key, "versao") == 0 || strcmp(key, "versão") == 0 ||
               strcmp(key, "version") == 0) {
        snprintf(d->versao, sizeof(d->versao), "%s", value);
    } else if (strcmp(key, "escopo") == 0 || strcmp(key, "scope") == 0) {
        snprintf(d->escopo, sizeof(d->escopo), "%s", value);
    } else if (strcmp(key, "status") == 0) {
        snprintf(d->status, sizeof(d->status), "%s", value);
    } else if (strcmp(key, "caminho") == 0 || strcmp(key, "path") == 0) {
        snprintf(d->caminho, sizeof(d->caminho), "%s", value);
    }
}

static void dep_registry_load(DependencyRegistry* reg) {
    memset(reg, 0, sizeof(*reg));
    FILE* f = fopen(DEP_REGISTRY_FILE, "r");
    if (!f) return;

    char line[DEP_PATH_MAX + 128];
    Dependency cur;
    bool have = false;
    memset(&cur, 0, sizeof(cur));

    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        char* trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
        if (*trimmed == '\0' || *trimmed == '#') continue;

        if (strcmp(trimmed, "---") == 0) {
            if (have && cur.nome[0] && reg->count < DEP_MAX_PACKAGES) {
                reg->itens[reg->count++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            have = false;
            continue;
        }

        char* eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trimmed;
        char* value = eq + 1;
        dep_trim(key);
        dep_trim(value);
        dep_registry_assign(&cur, key, value);
        have = true;
    }
    if (have && cur.nome[0] && reg->count < DEP_MAX_PACKAGES) {
        reg->itens[reg->count++] = cur;
    }
    fclose(f);
}

static bool dep_registry_save(const DependencyRegistry* reg) {
    const char* tmp = DEP_REGISTRY_FILE ".tmp";
    FILE* f = fopen(tmp, "w");
    if (!f) return false;

    fprintf(f, "# Independência.txt.ng\n");
    fprintf(f, "# Registro nativo da NG. Gerado automaticamente.\n");
    fprintf(f, "# Campos: nome, origem, url, versao, escopo, status, caminho\n\n");

    for (size_t i = 0; i < reg->count; i++) {
        const Dependency* d = &reg->itens[i];
        fprintf(f, "---\n");
        fprintf(f, "nome=%s\n", d->nome);
        fprintf(f, "origem=%s\n", d->origem);
        fprintf(f, "url=%s\n", d->url);
        fprintf(f, "versao=%s\n", d->versao);
        fprintf(f, "escopo=%s\n", d->escopo);
        fprintf(f, "status=%s\n", d->status);
        fprintf(f, "caminho=%s\n", d->caminho);
    }
    fclose(f);

    if (rename(tmp, DEP_REGISTRY_FILE) == 0) return true;
    unlink(DEP_REGISTRY_FILE);
    return rename(tmp, DEP_REGISTRY_FILE) == 0;
}

static size_t dep_registry_find(const DependencyRegistry* reg, const char* nome) {
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->itens[i].nome, nome) == 0) return i;
    }
    return (size_t)-1;
}

/* ------------------------------------------------------------------ */
/* Classificacao, nomes e versoes                                      */
/* ------------------------------------------------------------------ */

static bool dep_is_git_checkout(const char* path) {
    char probe[DEP_PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/.git", path);
    return dep_exists(probe);
}

static DepOrigin dep_classify(const char* src) {
    if (!src || !*src) return DEP_ORIGIN_NAME;
    if (strncmp(src, "git@", 4) == 0 || strncmp(src, "ssh://", 6) == 0 ||
        strstr(src, ".git") != NULL) {
        return DEP_ORIGIN_GIT;
    }
    if ((src[0] == '/' || strncmp(src, "./", 2) == 0) && dep_is_git_checkout(src)) {
        return DEP_ORIGIN_GIT;
    }
    if (strstr(src, "://") != NULL || src[0] == '/' || strncmp(src, "./", 2) == 0) {
        return DEP_ORIGIN_URL;
    }
    return DEP_ORIGIN_NAME;
}

static void dep_name_from_source(const char* src, char* out, size_t cap) {
    const char* p = src;
    if (strncmp(p, "file://", 7) == 0) p += 7;

    char tmp[DEP_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", p);
    char* cut = strpbrk(tmp, "?#");
    if (cut) *cut = '\0';
    size_t len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';

    const char* slash = strrchr(tmp, '/');
    const char* base = slash ? slash + 1 : tmp;
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".git") == 0) blen -= 4;
    else if (blen > 3 && strcmp(base + blen - 3, ".ng") == 0) blen -= 3;

    snprintf(out, cap, "%.*s", (int)blen, base);
    if (out[0] == '\0') snprintf(out, cap, "pacote");
    for (char* c = out; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') *c = '_';
    }
}

static char* dep_read_head(const char* path, size_t max) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* buf = (char*)malloc(max + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, max, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static char* dep_detect_version(const char* path, const char* fallback) {
    char source[DEP_PATH_MAX];
    snprintf(source, sizeof(source), "%s/index.ng", path);
    if (!dep_exists(source)) snprintf(source, sizeof(source), "%s", path);

    char* head = dep_read_head(source, 16384);
    if (head) {
        const char* keys[] = {"versao", "versão", "Versao", "Versão", "version", "Version"};
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            char* p = head;
            while ((p = strstr(p, keys[k])) != NULL) {
                char* eq = strchr(p, '=');
                if (eq && eq - p < 32) {
                    char* q = eq + 1;
                    while (*q && isspace((unsigned char)*q)) q++;
                    if (*q == '"' || *q == '\'') {
                        char quote = *q++;
                        char value[128];
                        size_t vi = 0;
                        while (*q && *q != quote && vi + 1 < sizeof(value)) value[vi++] = *q++;
                        value[vi] = '\0';
                        if (vi > 0) {
                            char* result = strdup(value);
                            free(head);
                            return result;
                        }
                    }
                }
                p++;
            }
        }
        free(head);
    }

    if (dep_is_git_checkout(path)) {
        char* git_argv[] = {(char*)"git", (char*)"-C", (char*)path,
                            (char*)"rev-parse", (char*)"--short", (char*)"HEAD", NULL};
        NGProcessResult r = ng_process_run(git_argv, NULL);
        if (r.output) {
            char* v = r.output;
            while (*v && isspace((unsigned char)*v)) v++;
            char* end = v + strlen(v);
            while (end > v && isspace((unsigned char)end[-1])) end--;
            *end = '\0';
            if (*v) {
                char* result = strdup(v);
                ng_process_result_free(&r);
                return result;
            }
        }
        ng_process_result_free(&r);
    }
    return strdup(fallback ? fallback : "0.0.0");
}

/* ------------------------------------------------------------------ */
/* Download / instalacao                                               */
/* ------------------------------------------------------------------ */

static bool dep_download(const char* url, const char* out_path) {
    if (strncmp(url, "file://", 7) == 0) {
        return dep_copy_file(url + 7, out_path);
    }

    char parent[DEP_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", out_path);
    char* slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        dep_mkdir_p(parent);
    }

#ifndef NG_NO_CURL
    NGHttpResponse* resp = ng_http_get(url, 30000);
    if (!resp) return false;
    bool ok = resp->status_code >= 200 && resp->status_code < 300;
    if (ok) {
        FILE* f = fopen(out_path, "wb");
        if (!f) {
            ok = false;
        } else {
            if (resp->body_len > 0) {
                fwrite(resp->body, 1, resp->body_len, f);
            }
            fclose(f);
        }
    }
    ng_http_response_free(resp);
    return ok;
#else
    /* Cross-compilacao Android nao tem libcurl: usa o binario curl. */
    char* argv[] = {(char*)"curl", (char*)"-fsSL", (char*)"--max-time", (char*)"30",
                    (char*)"-o", (char*)out_path, (char*)url, NULL};
    NGProcessResult r = ng_process_run(argv, NULL);
    bool ok = r.exit_code == 0 && dep_exists(out_path);
    if (!ok && r.output && *r.output) {
        fprintf(stderr, "%s", r.output);
    }
    ng_process_result_free(&r);
    return ok;
#endif
}

static bool dep_try_local_source(const char* name, const char* scope,
                                 char* found_path, size_t cap) {
    char root[DEP_PATH_MAX];
    dep_root_for(scope, root, sizeof(root));

    const char* patterns[] = {
        "%s/%s/index.ng",
        "%s/%s.ng",
        "libs/%s/index.ng",
        "libs/%s.ng"
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        char candidate[DEP_PATH_MAX];
        if (i < 2) snprintf(candidate, sizeof(candidate), patterns[i], root, name);
        else snprintf(candidate, sizeof(candidate), patterns[i], name);
        if (dep_exists(candidate)) {
            snprintf(found_path, cap, "%s", candidate);
            return true;
        }
    }
    return false;
}

static bool dep_install_by_name(const char* name, const char* scope, Dependency* rec) {
    char root[DEP_PATH_MAX];
    dep_root_for(scope, root, sizeof(root));
    char target_dir[DEP_PATH_MAX];
    snprintf(target_dir, sizeof(target_dir), "%s/%s", root, name);
    char target_file[DEP_PATH_MAX];
    snprintf(target_file, sizeof(target_file), "%s/index.ng", target_dir);

    char source[DEP_PATH_MAX];
    if (dep_try_local_source(name, scope, source, sizeof(source))) {
        if (strcmp(source, target_dir) != 0 && strcmp(source, target_file) != 0) {
            dep_remove_path(target_dir);
            dep_mkdir_p(target_dir);
            if (dep_is_dir(source)) {
                if (!dep_copy_dir(source, target_dir)) return false;
            } else if (!dep_copy_file(source, target_file)) {
                return false;
            }
        }
        snprintf(rec->url, sizeof(rec->url), "local:%s", source);
    } else {
        const char* base = getenv("NG_INDEPENDENCIA_REGISTRY");
        if (!base || !*base) base = DEP_DEFAULT_REGISTRY;
        char url1[DEP_PATH_MAX], url2[DEP_PATH_MAX];
        snprintf(url1, sizeof(url1), "%s/%s/index.ng", base, name);
        snprintf(url2, sizeof(url2), "%s/%s.ng", base, name);

        bool downloaded = false;
        if (strncmp(base, "file://", 7) == 0 || strstr(base, "://") != NULL) {
            downloaded = dep_download(url1, target_file);
            if (!downloaded) downloaded = dep_download(url2, target_file);
        }
        if (!downloaded) return false;

        dep_mkdir_p(target_dir);
        snprintf(rec->url, sizeof(rec->url), "%s", downloaded ? url1 : url2);
    }

    char* version = dep_detect_version(target_dir, "0.0.0");
    snprintf(rec->versao, sizeof(rec->versao), "%s", version ? version : "0.0.0");
    free(version);

    snprintf(rec->nome, sizeof(rec->nome), "%s", name);
    snprintf(rec->origem, sizeof(rec->origem), "nome");
    snprintf(rec->escopo, sizeof(rec->escopo), "%s", scope);
    snprintf(rec->status, sizeof(rec->status), "instalado");
    snprintf(rec->caminho, sizeof(rec->caminho), "%s", target_dir);
    return true;
}

static bool dep_install_from_source(const char* src, const char* scope, Dependency* rec) {
    char root[DEP_PATH_MAX];
    dep_root_for(scope, root, sizeof(root));
    dep_mkdir_p(root);

    char name[128];
    dep_name_from_source(src, name, sizeof(name));

    DepOrigin origin = dep_classify(src);
    char target[DEP_PATH_MAX];
    char target_file[DEP_PATH_MAX];

    if (origin == DEP_ORIGIN_URL &&
        (strstr(src, ".ng") != NULL || strstr(src, ".txt.ng") != NULL)) {
        snprintf(target, sizeof(target), "%s/%s.ng", root, name);
        snprintf(target_file, sizeof(target_file), "%s", target);
    } else {
        snprintf(target, sizeof(target), "%s/%s", root, name);
        snprintf(target_file, sizeof(target_file), "%s/index.ng", target);
    }

    dep_remove_path(target);

    if (origin == DEP_ORIGIN_GIT) {
        char* git_argv[] = {(char*)"git", (char*)"clone", (char*)"--depth", (char*)"1",
                            (char*)src, (char*)target, NULL};
        NGProcessResult r = ng_process_run(git_argv, NULL);
        bool ok = r.exit_code == 0;
        if (!ok) {
            fprintf(stderr, "[Independência] Falha ao clonar '%s'.\n", src);
            if (r.output && *r.output) fprintf(stderr, "%s", r.output);
        }
        ng_process_result_free(&r);
        if (!ok) return false;
    } else {
        if (!dep_download(src, target_file)) {
            fprintf(stderr, "[Independência] Falha ao baixar '%s'.\n", src);
            return false;
        }
    }

    char* version = dep_detect_version(target, "0.0.0");
    snprintf(rec->versao, sizeof(rec->versao), "%s", version ? version : "0.0.0");
    free(version);

    snprintf(rec->nome, sizeof(rec->nome), "%s", name);
    snprintf(rec->origem, sizeof(rec->origem), "%s",
             origin == DEP_ORIGIN_GIT ? "git" : "url");
    snprintf(rec->url, sizeof(rec->url), "%s", src);
    snprintf(rec->escopo, sizeof(rec->escopo), "%s", scope);
    snprintf(rec->status, sizeof(rec->status), "instalado");
    snprintf(rec->caminho, sizeof(rec->caminho), "%s", target);
    return true;
}

/* ------------------------------------------------------------------ */
/* Comandos da CLI                                                     */
/* ------------------------------------------------------------------ */

static void dep_print_usage(void) {
    printf("Independência — gerenciador de dependências da NG\n");
    printf("Uso:\n");
    printf("  ng independência install <pacote|URL> [-g]\n");
    printf("  ng des i <pacote|URL> [-g]\n");
    printf("  ng independência desinstalar <pacote|URL>\n");
    printf("  ng des des <pacote|URL>\n");
    printf("  ng independência list\n");
    printf("  ng des list\n");
}

static void dep_print_list(const DependencyRegistry* reg) {
    if (reg->count == 0) {
        printf("[Independência] Nenhum pacote registrado em %s.\n", DEP_REGISTRY_FILE);
        return;
    }
    printf("%-22s %-8s %-12s %-10s %-10s %s\n",
           "NOME", "ORIGEM", "VERSAO", "ESCOPO", "STATUS", "URL");
    printf("--------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < reg->count; i++) {
        const Dependency* d = &reg->itens[i];
        printf("%-22s %-8s %-12s %-10s %-10s %s\n",
               d->nome, d->origem, d->versao, d->escopo, d->status, d->url);
    }
}

int ng_dependency_command(int argc, char** argv) {
    if (argc < 1) {
        dep_print_usage();
        return 1;
    }

    const char* action = argv[0];
    bool global = false;
    const char* target = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0) {
            global = true;
        } else if (!target) {
            target = argv[i];
        }
    }

    if (strcmp(action, "list") == 0 || strcmp(action, "lista") == 0) {
        DependencyRegistry reg;
        dep_registry_load(&reg);
        dep_print_list(&reg);
        return 0;
    }

    bool is_install = strcmp(action, "install") == 0 || strcmp(action, "i") == 0;
    bool is_remove = strcmp(action, "desinstalar") == 0 || strcmp(action, "des") == 0 ||
                     strcmp(action, "uninstall") == 0 || strcmp(action, "rm") == 0;

    if (!is_install && !is_remove) {
        fprintf(stderr, "[Independência] Comando desconhecido: '%s'.\n", action);
        dep_print_usage();
        return 1;
    }
    if (!target) {
        fprintf(stderr, "[Independência] Informe o pacote, URL ou caminho.\n");
        dep_print_usage();
        return 1;
    }

    DependencyRegistry reg;
    dep_registry_load(&reg);

    if (is_install) {
        Dependency rec;
        memset(&rec, 0, sizeof(rec));
        const char* scope = global ? "global" : "local";
        bool ok;

        DepOrigin origin = dep_classify(target);
        if (origin == DEP_ORIGIN_NAME) {
            ok = dep_install_by_name(target, scope, &rec);
        } else {
            ok = dep_install_from_source(target, scope, &rec);
        }

        if (!ok) {
            fprintf(stderr, "[Independência] Não foi possível instalar '%s'.\n", target);
            return 1;
        }

        size_t idx = dep_registry_find(&reg, rec.nome);
        if (idx == (size_t)-1) {
            if (reg.count >= DEP_MAX_PACKAGES) {
                fprintf(stderr, "[Independência] Registro cheio.\n");
                return 1;
            }
            idx = reg.count++;
        }
        reg.itens[idx] = rec;

        if (!dep_registry_save(&reg)) {
            fprintf(stderr, "[Independência] Falha ao gravar %s.\n", DEP_REGISTRY_FILE);
            return 1;
        }

        printf("[Independência] '%s' instalado (%s, %s, v%s).\n",
               rec.nome, rec.origem, rec.escopo, rec.versao);
        return 0;
    }

    /* desinstalar */
    size_t idx = dep_registry_find(&reg, target);
    if (idx == (size_t)-1) {
        char derived[128];
        dep_name_from_source(target, derived, sizeof(derived));
        idx = dep_registry_find(&reg, derived);
    }
    if (idx == (size_t)-1) {
        fprintf(stderr, "[Independência] Pacote '%s' não está registrado.\n", target);
        return 1;
    }

    Dependency* d = &reg.itens[idx];
    dep_remove_path(d->caminho);
    snprintf(d->status, sizeof(d->status), "removido");

    if (!dep_registry_save(&reg)) {
        fprintf(stderr, "[Independência] Falha ao atualizar %s.\n", DEP_REGISTRY_FILE);
        return 1;
    }
    printf("[Independência] '%s' desinstalado. Registro atualizado.\n", d->nome);
    return 0;
}
