#define _POSIX_C_SOURCE 200809L
#include "ng_process.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

void ng_process_result_free(NGProcessResult* result) {
    if (!result) return;
    free(result->output);
    result->output = NULL;
    result->exit_code = -1;
}
