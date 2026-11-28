#!/bin/bash
# Testes do JIT nativo: output JIT == output interpretador + valores esperados.
NG=${1:-./bin/ng}
fails=0
errf=$(mktemp)
out=$(NG_JIT_DEBUG=1 "$NG" executar testes/jit_full.ng 2>"$errf"); rc1=$?
out2=$(NG_JIT_DISABLE=1 "$NG" executar testes/jit_full.ng 2>/dev/null); rc2=$?
if ! grep -q "\[JIT\] ok" "$errf"; then echo "FAIL: JIT nao rodou (fallback?)"; cat "$errf"; fails=1; fi
rm -f "$errf"
if [ $rc1 -ne 0 ]; then echo "FAIL: executar JIT rc=$rc1"; echo "$out"; fails=1; fi
if [ $rc2 -ne 0 ]; then echo "FAIL: executar interp rc=$rc2"; echo "$out2"; fails=1; fi
diff <(echo "$out") <(echo "$out2") >/dev/null && { echo "PASS: saida JIT == interpretador"; } || { echo "FAIL: saidas diferem"; diff <(echo "$out") <(echo "$out2") | head; fails=1; }
echo "$out" | grep -q "^17$" && echo "PASS: X == 17" || { echo "FAIL: X != 17"; fails=1; }
echo "$out" | grep -q "^16$" && echo "PASS: Y2 == 16" || { echo "FAIL: Y2 != 16"; fails=1; }
echo "$out" | grep -q "^42$" && echo "PASS: Dobro(21) == 42" || { echo "FAIL: Dobro(21) != 42"; fails=1; }
echo "$out" | grep -q "^7$" && echo "PASS: Soma(3,4) == 7" || { echo "FAIL: Soma(3,4) != 7"; fails=1; }
echo "$out" | grep -q "^5$" && echo "PASS: Total == 5" || { echo "FAIL: Total != 5"; fails=1; }
echo "$out" | grep -q "^100$" && echo "PASS: Se 7>3 -> 100" || { echo "FAIL: Se 7>3"; fails=1; }
echo "$out" | grep -q "^300$" && echo "PASS: Se 0==0 -> 300" || { echo "FAIL: Se 0==0"; fails=1; }
echo "$out" | grep -q "3.14159" && echo "PASS: PI == 3.14159" || { echo "FAIL: PI"; fails=1; }
exit $fails
