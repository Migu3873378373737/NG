#!/bin/bash
# Testes da biblioteca JSON. Uso: bash testes/test_json.sh [binario]
set -u

BIN="${1:-bin/ng}"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR/.."

OUT="$($BIN executar testes/29_json_basico.ng 2>&1)"
echo "$OUT" | head -40

echo "$OUT" | grep -q "São Paulo" || { echo "FALHOU: UTF-8/Definir"; exit 1; }
echo "$OUT" | grep -q '"nome": "João"' || { echo "FALHOU: formatacao JSON"; exit 1; }
echo "$OUT" | grep -q "3" || { echo "FALHOU: tamanho"; exit 1; }

echo "Todos os testes JSON passaram."
