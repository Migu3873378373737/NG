#!/bin/bash
# Testes SQLite reais. Uso: bash testes/test_sqlite.sh [binario]
set -u

BIN="${1:-bin/ng}"
DIR="$(cd "$(dirname "$0")" && pwd)"

for f in 25_sqlite_basico.ng 26_sqlite_avancado.ng 27_sqlite_parametros.ng; do
    echo "== $f"
    "$BIN" executar "$DIR/$f" > /dev/null 2>&1 || { echo "FALHOU: $f"; exit 1; }
done

OUT="$($BIN executar "$DIR/28_sqlite_erros.ng" 2>&1)"
echo "$OUT" | grep -q "NG-401" || { echo "FALHOU: erro SQL nao reportado"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "NG-110" || { echo "FALHOU: exclusao insegura nao bloqueada"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "antes do erro" || { echo "FALHOU: execucao anterior perdida"; exit 1; }
echo "$OUT" | grep -q "depois do erro SQL" || { echo "FALHOU: loop interrompido por erro SQL"; exit 1; }

echo "Todos os testes SQLite passaram."
