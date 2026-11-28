#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ng_process.h"
#else
#define _POSIX_C_SOURCE 200809L
#include "ng_process.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static bool process_grow(char** buf, size_t* len, size_t* cap, size_t extra) {
    if (*len + extra + 1 <= *cap) return true;
    size_t new_cap = *cap > 0 ? *cap : 1024;
    while (*len + extra + 1 > new_cap) new_cap *= 2;
    char* p = (char*)realloc(*buf, new_cap);
    if (!p) return false;
    *buf = p;
    *cap = new_cap;
    return true;
}

#ifdef _WIN32

/* Win32: execucao de processo com pipe de stdout+stderr (CreateProcess).
   Equivalente ao fluxo fork+execvp+pipe do POSIX, preservando argumentos,
   redirecionamento, exit code e captura de stdout/stderr. */
NGProcessResult ng_process_run(char* const argv[], const char* env_value) {
    NGProcessResult result;
    result.output = NULL;
    result.exit_code = -1;

    if (!argv || !argv[0]) return result;

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    HANDLE hNull = NULL;
    HANDLE hProcess = NULL;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    if (!CreatePipe(&hReadPipe, &hWritePipe, NULL, 0)) return result;
    hNull = CreateFileA("NUL", GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!hNull) { CloseHandle(hReadPipe); CloseHandle(hWritePipe); return result; }

    STARTUPINFOA siex;
    memset(&siex, 0, sizeof(siex));
    siex.cb = sizeof(STARTUPINFOA);
    siex.dwFlags = STARTF_USESTDHANDLES;
    siex.hStdInput = hNull;
    siex.hStdOutput = hWritePipe;
    siex.hStdError = hWritePipe;

    /* Monta lpCommandLine com aspas em torno de cada argumento. */
    char cmdline[8192];
    {
        size_t olen = 0;
        for (int i = 0; argv[i]; i++) {
            char tmpbuf[1024];
            int tl = snprintf(tmpbuf, sizeof(tmpbuf), "\"%s\"", argv[i]);
            if (tl <= 0 || (size_t)tl >= sizeof(tmpbuf)) break;
            size_t need = (i > 0 ? 1 : 0) + (size_t)tl + 1;
            if (olen + need > sizeof(cmdline)) break;
            if (i > 0) cmdline[olen++] = ' ';
            memcpy(cmdline + olen, tmpbuf, (size_t)tl);
            olen += (size_t)tl;
        }
        cmdline[olen] = '\0';
    }

    LPVOID env_block = NULL;
    if (env_value && env_value[0]) {
        char envbuf[1024];
        snprintf(envbuf, sizeof(envbuf), "NG_INPUT=%s", env_value);
        env_block = envbuf;
    }

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, env_block, NULL, &siex, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        CloseHandle(hNull);
        return result;
    }

    CloseHandle(pi.hThread);
    CloseHandle(hNull);
    CloseHandle(hWritePipe);

    /* Le a pipe ate o EOF (o filho fechou a escrita ao sair). */
    size_t cap = 0;
    size_t len = 0;
    char* buf = NULL;
    char chunk[4096];
    DWORD nread = 0;
    BOOL ok = TRUE;
    while (ok && ReadFile(hReadPipe, chunk, sizeof(chunk), &nread, NULL) && nread > 0) {
        if (!process_grow(&buf, &len, &cap, nread)) { ok = FALSE; break; }
        memcpy(buf + len, chunk, nread);
        len += nread;
        buf[len] = '\0';
    }
    CloseHandle(hReadPipe);

    /* Aguarda o processo terminar e captura o exit code. */
    DWORD exit_code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0) {
        if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != (DWORD)0xFFFFFFFF) {
            result.exit_code = (int)exit_code;
        }
    }
    CloseHandle(pi.hProcess);

    if (!buf) {
        buf = (char*)malloc(1);
        if (buf) buf[0] = '\0';
    }
    result.output = buf;
    return result;
}

#else /* _WIN32 */

NGProcessResult ng_process_run(char* const argv[], const char* env_value) {
    NGProcessResult result;
    result.output = NULL;
    result.exit_code = -1;

    if (!argv || !argv[0]) return result;

    int pipefd[2];
    if (pipe(pipefd) != 0) return result;

    if (env_value) {
        setenv("NG_INPUT", env_value, 1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (env_value) unsetenv("NG_INPUT");
        return result;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    size_t cap = 0;
    size_t len = 0;
    char* buf = NULL;
    char chunk[4096];

    for (;;) {
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n > 0) {
            if (!process_grow(&buf, &len, &cap, (size_t)n)) break;
            memcpy(buf + len, chunk, (size_t)n);
            len += (size_t)n;
            buf[len] = '\0';
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (env_value) unsetenv("NG_INPUT");

    if (!buf) {
        buf = (char*)malloc(1);
        if (buf) buf[0] = '\0';
    }
    result.output = buf;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif /* _WIN32 */

void ng_process_result_free(NGProcessResult* result) {
    if (!result) return;
    free(result->output);
    result->output = NULL;
    result->exit_code = -1;
}
