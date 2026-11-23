#!/bin/bash
# Testes funcionais do Dynamic V Dual-Core (CLI + bench + semântica).
# Uso: bash testes/test_dynamicv.sh [./bin/ng]
NG="${1:-./bin/ng}"
fails=0
check() {
    if [ "$1" = "0" ]; then echo "PASS: $2"; else echo "FAIL: $2"; fails=$((fails+1)); fi
}

out=$("$NG" dynamicv status)
echo "$out" | grep -q "ATIVADO"; check $? "status: motor ATIVADO"
echo "$out" | grep -q "Core 1 (Lua Worker):  Ocioso / Pronto (Worker thread: SIM)"; check $? "status: worker Lua SIM"

out=$("$NG" dynamicv cache)
echo "$out" | grep -q "Diretório de Cache"; check $? "cache: informação do N3"

out=$("$NG" dynamicv limpar)
echo "$out" | grep -q "limpo"; check $? "limpar: cache limpo"

out=$("$NG" dynamicv diagnostico)
echo "$out" | grep -q "Benchmark Dynamic V"; check $? "diagnostico: inclui benchmark"
echo "$out" | grep -qE "Miss \(parse\+Core1\): [0-9.]+ ms/it"; check $? "diagnostico: bench miss medido (ms/it)"
echo "$out" | grep -qE "Hit  \(N3 Load\):     [0-9.]+ ms/it"; check $? "diagnostico: bench hit medido (ms/it)"

out=$("$NG" executar testes/dv_semantics.ng)
echo "$out" | grep -q "42"; check $? "semantica: Dobro(21) imprime 42"
echo "$out" | grep -q "49"; check $? "semantica: Quadrado(7) imprime 49"

"$NG" executar testes/dv_semantics.ng > /dev/null 2>&1
out=$("$NG" dynamicv cache)
echo "$out" | grep -qE "Total de V-units:     [1-9]"; check $? "N3: entrada populada após executar"

NG_DYNAMICV=0 "$NG" executar testes/dv_semantics.ng > /dev/null 2>&1
check $? "fallback C (NG_DYNAMICV=0): dv_semantics executa"

exit $fails
