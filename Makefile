CC ?= clang
# Otimizacao de tamanho/velocidade: -Os + LTO + secoes separadas e strip.
# Sobrescreva com: make OPT_LEVEL=-O2 LTO=
OPT_LEVEL ?= -Os
LTO ?= -flto
CFLAGS = -D_GNU_SOURCE -Wall -Wextra -std=c11 $(OPT_LEVEL) $(LTO) -Iinclude \
         -ffunction-sections -fdata-sections \
         -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS = -lm -lpthread $(LTO) -Wl,--gc-sections -Wl,-s

OPENSSL_INC ?= /data/data/com.termux/files/usr/include
OPENSSL_LIB ?= /data/data/com.termux/files/usr/lib

# Detecção automatica do OpenSSL para HTTPS nativo (opcional)
ifeq ($(shell test -f $(OPENSSL_INC)/openssl/ssl.h && echo yes),yes)
    CFLAGS += -DNG_HAS_OPENSSL -I$(OPENSSL_INC)
    LDFLAGS += -L$(OPENSSL_LIB) -lssl -lcrypto
else ifeq ($(shell test -f /usr/include/openssl/ssl.h && echo yes),yes)
    CFLAGS += -DNG_HAS_OPENSSL
    LDFLAGS += -lssl -lcrypto
endif

# libcurl e usado pelo cliente HTTP nativo (tambem pelo Independencia)
CURL_INC ?= /data/data/com.termux/files/usr/include
CURL_LIB ?= /data/data/com.termux/files/usr/lib
ifeq ($(shell test -f $(CURL_INC)/curl/curl.h && echo yes),yes)
    CFLAGS += -I$(CURL_INC)
    LDFLAGS += -L$(CURL_LIB) -lcurl
else ifeq ($(shell test -f /usr/include/curl/curl.h && echo yes),yes)
    LDFLAGS += -lcurl
endif

# SQLite nativo (opcional: sem ele o modulo @sqlite reporta indisponibilidade)
SQLITE_INC ?= /data/data/com.termux/files/usr/include
SQLITE_LIB ?= /data/data/com.termux/files/usr/lib
ifeq ($(shell test -f $(SQLITE_INC)/sqlite3.h && echo yes),yes)
    CFLAGS += -I$(SQLITE_INC)
    LDFLAGS += -L$(SQLITE_LIB) -lsqlite3
else ifeq ($(shell test -f /usr/include/sqlite3.h && echo yes),yes)
    LDFLAGS += -lsqlite3
else
    CFLAGS += -DNG_NO_SQLITE
endif

SRC = src/main.c src/lexer.c src/parser.c src/ast.c src/runtime.c src/value.c src/interpreter.c src/module.c src/ng_native_module.c src/ng_json.c src/ng_http.c src/ng_http_client.c src/ng_net_libs.c src/ng_transpiler_lua.c src/ng_stdlib.c src/ng_n3cache.c src/repl.c src/ng_process.c src/ng_dependency.c src/ng_interop.c src/ng_collection.c src/ng_sqlite.c src/ng_print.c src/ng_dynamicv.c src/ng_l2.c src/ng_ir.c src/ng_lua_optimizer.c src/ng_outbuf.c src/ng_jit.c
BINDIR = bin
TARGET = $(BINDIR)/ng
# Em Android/Termux o /storage costuma ser noexec: os testes rodam uma copia
# executavel em $PREFIX/tmp (ou /tmp fora do Termux).
TMPRUN ?= $(if $(PREFIX),$(PREFIX)/tmp/ng-run,/tmp/ng-run)

# Deteccao automatica do Lua embedded (Core 1 do Dynamic V) via API C.
# Termux: /data/data/com.termux/files/usr/include/lua5.4; Linux: /usr/include/lua.h
LUA_INC ?= /data/data/com.termux/files/usr/include
LUA_LIB ?= /data/data/com.termux/files/usr/lib
ifneq ($(wildcard $(LUA_INC)/lua5.4/lua.h),)
    CFLAGS += -DNG_HAS_EMBEDDED_LUA -I$(LUA_INC)/lua5.4
    LDFLAGS += -L$(LUA_LIB) -llua5.4 -Wl,-rpath,$(LUA_LIB)
else ifneq ($(wildcard $(LUA_INC)/lua5.1/lua.h),)
    CFLAGS += -DNG_HAS_EMBEDDED_LUA -I$(LUA_INC)/lua5.1
    LDFLAGS += -L$(LUA_LIB) -llua5.1 -Wl,-rpath,$(LUA_LIB)
else ifneq ($(wildcard $(LUA_INC)/lua.h),)
    CFLAGS += -DNG_HAS_EMBEDDED_LUA -I$(LUA_INC)
    LDFLAGS += -L$(LUA_LIB) -llua -Wl,-rpath,$(LUA_LIB)
else ifneq ($(wildcard /usr/include/lua.h),)
    CFLAGS += -DNG_HAS_EMBEDDED_LUA
    LDFLAGS += -llua
else
    $(info [Makefile] Lua embedded nao detectado: Dynamic V rodara no modo C (fallback Core 0))
endif

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@echo "Build completo: bin/ng gerado com sucesso."

$(TMPRUN): $(TARGET)
	@mkdir -p $(dir $(TMPRUN))
	@cp $(TARGET) $(TMPRUN) && chmod +x $(TMPRUN)

clean:
	rm -rf $(BINDIR)

test: $(TMPRUN)
	@echo "== Executando Testes NG =="
	@$(TMPRUN) executar testes/01_ola_mundo.ng
	@$(TMPRUN) executar testes/02_variaveis.ng
	@$(TMPRUN) executar testes/03_g_y.ng
	@$(TMPRUN) executar testes/04_condicoes.ng
	@$(TMPRUN) executar testes/05_funcoes.ng
	@$(TMPRUN) executar testes/06_json.ng
	@$(TMPRUN) executar testes/07_http_servidor.ng
	@$(TMPRUN) executar testes/08_http_cliente.ng
	@$(TMPRUN) executar testes/09_http_express.ng
	@$(TMPRUN) executar testes/16_loop_um.ng
	@$(TMPRUN) executar testes/17_loop_cinco.ng
	@$(TMPRUN) executar testes/18_loop_muitos.ng
	@$(TMPRUN) executar testes/19_lista_vazia.ng
	@$(TMPRUN) executar testes/20_loop_acentos.ng
	@$(TMPRUN) executar testes/21_print_lista.ng
	@$(TMPRUN) executar testes/22_loop_aninhado.ng
	@$(TMPRUN) executar testes/25_sqlite_basico.ng
	@$(TMPRUN) executar testes/26_sqlite_avancado.ng
	@$(TMPRUN) executar testes/27_sqlite_parametros.ng
	@$(TMPRUN) executar testes/29_json_basico.ng
	@$(TMPRUN) executar testes/30_print_avancado.ng
	@$(TMPRUN) executar testes/31_info_diag.ng
	@$(TMPRUN) executar testes/32_correcoes.ng
	@echo "== Todos os testes concluidos com sucesso! =="

test-loop: $(TMPRUN)
	@bash testes/test_loop.sh $(TMPRUN)

test-sqlite: $(TMPRUN)
	@bash testes/test_sqlite.sh $(TMPRUN)

test-json: $(TMPRUN)
	@bash testes/test_json.sh $(TMPRUN)

test-print: $(TMPRUN)
	@bash testes/test_print.sh $(TMPRUN)

test-all: test-loop test-sqlite test-json test-print

bench: $(TMPRUN)
	@$(TMPRUN) executar testes/bench_long.ng
