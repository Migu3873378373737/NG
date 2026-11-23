#!/bin/bash
# Testes de Print avancado, Info e diagnosticos. Uso: bash testes/test_print.sh [binario]
set -u

BIN="${1:-bin/ng}"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR/.."

OUT="$($BIN executar testes/30_print_avancado.ng 2>&1)"
echo "$OUT" | head -50
echo "$OUT" | grep -q "Ana,João,42,7" || { echo "FALHOU: CSV"; exit 1; }
echo "$OUT" | grep -q '"João"' || { echo "FALHOU: JSON indentado"; exit 1; }
echo "$OUT" | grep -q "1. Ana" || { echo "FALHOU: enumerado"; exit 1; }
echo "$OUT" | grep -q "0xFF" || { echo "FALHOU: hex"; exit 1; }
echo "$OUT" | grep -q "^64$" || { echo "FALHOU: quantidade de modos"; exit 1; }

OUT2="$($BIN executar testes/31_info_diag.ng 2>&1)"
echo "$OUT2" | head -50
echo "$OUT2" | grep -q "NG Language v" || { echo "FALHOU: versao"; exit 1; }
echo "$OUT2" | grep -q "aarch64" || { echo "FALHOU: arquitetura"; exit 1; }
echo "$OUT2" | grep -q "clang" || { echo "FALHOU: compilador"; exit 1; }
echo "$OUT2" | grep -q "condicao nao foi satisfeita" || { echo "FALHOU: diagnostico"; exit 1; }

echo "Todos os testes de Print/Info/Diag passaram."
