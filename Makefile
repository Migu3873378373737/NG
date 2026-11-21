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

SRC = src/main.c src/lexer.c src/parser.c src/ast.c src/runtime.c src/value.c src/interpreter.c src/module.c src/ng_native_module.c src/ng_json.c src/ng_http.c src/ng_http_client.c src/ng_net_libs.c src/ng_stdlib.c src/ng_n3cache.c src/repl.c src/ng_process.c src/ng_dependency.c src/ng_interop.c src/ng_collection.c src/ng_sqlite.c src/ng_print.c
BINDIR = bin
TARGET = $(BINDIR)/ng

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@echo "Build completo: bin/ng gerado com sucesso."

clean:
	rm -rf $(BINDIR)

test: $(TARGET)
	@echo "== Executando Testes NG =="
	@./$(TARGET) executar testes/01_ola_mundo.ng
	@./$(TARGET) executar testes/02_variaveis.ng
	@./$(TARGET) executar testes/03_g_y.ng
	@./$(TARGET) executar testes/04_condicoes.ng
	@./$(TARGET) executar testes/05_funcoes.ng
	@./$(TARGET) executar testes/06_json.ng
	@./$(TARGET) executar testes/07_http_servidor.ng
	@./$(TARGET) executar testes/08_http_cliente.ng
	@./$(TARGET) executar testes/09_http_express.ng
	@./$(TARGET) executar testes/16_loop_um.ng
	@./$(TARGET) executar testes/17_loop_cinco.ng
	@./$(TARGET) executar testes/18_loop_muitos.ng
	@./$(TARGET) executar testes/19_lista_vazia.ng
	@./$(TARGET) executar testes/20_loop_acentos.ng
	@./$(TARGET) executar testes/21_print_lista.ng
	@./$(TARGET) executar testes/22_loop_aninhado.ng
	@./$(TARGET) executar testes/25_sqlite_basico.ng
	@./$(TARGET) executar testes/26_sqlite_avancado.ng
	@./$(TARGET) executar testes/27_sqlite_parametros.ng
	@./$(TARGET) executar testes/29_json_basico.ng
	@./$(TARGET) executar testes/30_print_avancado.ng
	@./$(TARGET) executar testes/31_info_diag.ng
	@echo "== Todos os testes concluidos com sucesso! =="

test-loop: $(TARGET)
	@bash testes/test_loop.sh ./$(TARGET)

test-sqlite: $(TARGET)
	@bash testes/test_sqlite.sh ./$(TARGET)

test-json: $(TARGET)
	@bash testes/test_json.sh ./$(TARGET)

test-print: $(TARGET)
	@bash testes/test_print.sh ./$(TARGET)

test-all: test-loop test-sqlite test-json test-print

bench: $(TARGET)
	@./$(TARGET) executar testes/bench_long.ng
