#!/bin/bash
# Testes de Loop/listas. Uso: bash testes/test_loop.sh [caminho-do-binario]
set -u

BIN="${1:-bin/ng}"
DIR="$(cd "$(dirname "$0")" && pwd)"

run_ok() {
    local file="$1"
    echo "== $file"
    "$BIN" executar "$DIR/$file" > /dev/null 2>&1 || {
        echo "FALHOU: $file"; return 1;
    }
}

echo "== Testes validos de Loop/listas"
run_ok 16_loop_um.ng || exit 1
run_ok 17_loop_cinco.ng || exit 1
run_ok 18_loop_muitos.ng || exit 1
run_ok 19_lista_vazia.ng || exit 1
run_ok 20_loop_acentos.ng || exit 1
run_ok 21_print_lista.ng || exit 1
run_ok 22_loop_aninhado.ng || exit 1

echo "== Erros esperados (nao devem travar o runtime)"

ERR_OUT="$($BIN executar "$DIR/23_loop_erro_tipo.ng" 2>&1)"
if ! echo "$ERR_OUT" | grep -q "NG-108"; then
    echo "FALHOU: erro NG-108 nao reportado"; echo "$ERR_OUT"; exit 1;
fi
if ! echo "$ERR_OUT" | grep -q "apos erro"; then
    echo "FALHOU: execucao nao continuou apos erro de tipo"; echo "$ERR_OUT"; exit 1;
fi

ERR_OUT="$($BIN executar "$DIR/24_loop_erro_sintaxe.ng" 2>&1)"
if ! echo "$ERR_OUT" | grep -q "NG-106"; then
    echo "FALHOU: erro NG-106 nao reportado"; echo "$ERR_OUT"; exit 1;
fi

echo "Todos os testes de Loop/listas passaram."
