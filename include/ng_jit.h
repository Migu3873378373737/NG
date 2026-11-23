#ifndef NG_JIT_H
#define NG_JIT_H

#include "ng_ast.h"
#include "ng_ir.h"
#include "ng_interpreter.h"
#include <stdbool.h>

/* Dynamic V "Core 0b": JIT nativo (aarch64) para hot paths/funções/loops.
 * Compila IR -> código de máquina executável; fallback seguro para o
 * interpretador quando não se aplica. */

/* Executa o programa via JIT se possível. Retorna false quando JIT não se
 * aplica (incompatível, sem RWX, desativado) -> caller usa interpretador. */
bool ng_jit_run_program(const NGAstNode* program, NGInterpreter* interp);

/* Desativa JIT (env NG_JIT_DISABLE=1). Retorna true se o JIT rodou. */
bool ng_jit_enabled(void);

#endif /* NG_JIT_H */
