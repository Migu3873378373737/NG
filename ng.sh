#!/data/data/com.termux/files/usr/bin/bash

# Diretorio do proprio projeto (o mount /storage costuma ser noexec).
PROJ="$(cd "$(dirname "$0")" && pwd)"
RUN="$PREFIX/tmp/ng-run"

mkdir -p "$PREFIX/tmp"

if [ ! -f "$PROJ/bin/ng" ]; then
    echo "[NG] Executável não encontrado. Compile primeiro."
    exit 1
fi

# Desabilita o compilador JIT para evitar erros de memória/mmap RWX
# que fazem o Termux matar o processo (OOM).
export NG_JIT_DISABLE=1

cp "$PROJ/bin/ng" "$RUN"
chmod +x "$RUN"

exec "$RUN" "$@"
