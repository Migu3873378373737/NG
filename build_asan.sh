#!/bin/bash
# Build ASAN+UBSan (sem LTO) para validação de memória do Dynamic V.
cd "$(dirname "$0")"
SRC="src/main.c src/lexer.c src/parser.c src/ast.c src/runtime.c src/value.c src/interpreter.c src/module.c src/ng_native_module.c src/ng_json.c src/ng_http.c src/ng_http_client.c src/ng_net_libs.c src/ng_transpiler_lua.c src/ng_stdlib.c src/ng_n3cache.c src/repl.c src/ng_process.c src/ng_dependency.c src/ng_interop.c src/ng_collection.c src/ng_sqlite.c src/ng_print.c src/ng_dynamicv.c src/ng_l2.c src/ng_ir.c src/ng_lua_optimizer.c src/ng_outbuf.c src/ng_jit.c"
TERMUX=/data/data/com.termux/files/usr
clang -D_GNU_SOURCE -Wall -Wextra -std=c11 -g -O1 -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude -I$TERMUX/include/lua5.4 -I$TERMUX/include \
  $SRC -o $TERMUX/tmp/opencode/ng_asan \
  -L$TERMUX/lib -lssl -lcrypto -lcurl -lsqlite3 -llua5.4 -Wl,-rpath,$TERMUX/lib \
  -lm -lpthread -Wl,--no-gc-sections 2>&1 | grep -E "error|warning" | head -30
echo "asan build done: $?"
