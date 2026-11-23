#ifndef NG_REPL_H
#define NG_REPL_H

#define NG_REPL_VERSION "v0.3.0-alpha"

/*
 * Inicia o REPL interativo da NG.
 *
 * Le a entrada de stdin (fgets), executa cada linha/programa no mesmo
 * NGInterpreter e retorna 0 ao encerrar normalmente (comando de saida ou EOF).
 */
int ng_repl_run(void);

#endif // NG_REPL_H
