#!/data/data/com.termux/files/usr/bin/bash

# Diretorio do proprio projeto (o mount /storage costuma ser noexec).
PROJ="$(cd "$(dirname "$0")" && pwd)"
RUN="$PREFIX/tmp/ng-run"

mkdir -p "$PREFIX/tmp"

if [ ! -f "$PROJ/bin/ng" ]; then
    echo "[NG] Executável não encontrado. Compile primeiro."
    exit 1
fi

cp "$PROJ/bin/ng" "$RUN"
chmod +x "$RUN"

exec "$RUN" "$@"
