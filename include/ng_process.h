#ifndef NG_PROCESS_H
#define NG_PROCESS_H

#include <stdbool.h>

/* Resultado de um processo externo executado sem shell (fork/execvp). */
typedef struct {
    char* output;    /* saida combinada de stdout+stderr (malloc) */
    int exit_code;   /* codigo de saida, ou -1 se falha ao iniciar */
} NGProcessResult;

/*
 * Executa argv (vetor terminado em NULL) com execvp.
 * Se env_value != NULL, define NG_INPUT=<env_value> apenas no filho.
 */
NGProcessResult ng_process_run(char* const argv[], const char* env_value);
void ng_process_result_free(NGProcessResult* result);

#endif /* NG_PROCESS_H */
