# 📘 Documentação Oficial da Linguagem **NG (Next Generation)**

> **Linguagem de programação independente**, com identidade sintática própria,
> interpretador nativo escrito em **C puro (padrão ISO C11)**, AST modular e CLI
> próprio.

**Versão atual do engine:** `NG Language Engine v0.3.0-alpha (C11 Native Runtime)`

---

## 📑 Sumário

1. [O que é a NG?](#1-o-que-é-a-ng)
2. [Instalação](#2-instalação)
   - [2.1 Pré-requisitos comuns](#21-pré-requisitos-comuns)
   - [2.2 Linux](#22-linux)
   - [2.3 Termux (Android)](#23-termux-android)
   - [2.4 Docker](#24-docker)
   - [2.5 Windows (sem PowerShell)](#25-windows-sem-powershell)
3. [Compilação](#3-compilação)
4. [CLI do binário `ng`](#4-cli-do-binário-ng)
5. [Como a NG funciona — instrução por instrução (tim-tim por tim-tim)](#5-como-a-ng-funciona--instrução-por-instrução-tim-tim-por-tim-tim)
6. [Referência de sintaxe](#6-referência-de-sintaxe)
7. [Tipos e valores](#7-tipos-e-valores)
8. [Variáveis, escopos e funções](#8-variáveis-escolos-e-funções)
9. [Módulos nativos](#9-módulos-nativos)
10. [Bibliotecas e superfícies de API](#10-bibliotecas-e-superfícies-de-api)
11. [Códigos de erro](#11-códigos-de-erro)
12. [REPL interativo](#12-repl-interativo)
13. [Dynamic V, N3 Cache, JIT e Backend Lua](#13-dynamic-v-n3-cache-jit-e-backend-lua)
14. [Testes](#14-testes)
15. [Desempenho e otimizações](#15-desempenho-e-otimizações)
16. [Exemplos completos](#16-exemplos-completos)
17. [Perguntas frequentes](#17-perguntas-frequentes)

---

## 1. O que é a NG?

A **NG** é uma linguagem de programação *full-stack* em construção, com:

- **Identidade sintática própria**: palavras reservadas em português/inglês
  (`Print`, `G`, `Y`, `Se`, `Senao`, `Enquanto`, `Loop`, `Funcao`, `Importar`),
  blocos delimitados por `[ ... ]`, retorno semântico via `Y`, canal de saída
  via `G`.
- **Interpretador nativo em C11** (padrão ISO C11), sem runtime de script
  embutido: lexer, parser, AST, runtime de valores e interpretador escritos em
  C puro. O build usa `clang` com otimização `-Os`, LTO e `--gc-sections`.
- **AST modular**: cada nó da árvore é um tipo próprio (`NG_AST_*`), permitindo
  extensão sem reescrever o interpretador.
- **CLI próprio** (`ng`) com subcomandos: `executar`, `verificar`, `compilar`,
  `limpar-cache`, `dynamicv`, `independencia`, `versao`, `ajuda`.

### 1.1 Conceitos centrais

| Conceito | Significado |
|---|---|
| **`Print <expr>`** | Imprime o valor de `<expr>` (ou o valor inteligente de listas/tabelas). |
| **`Y`** | Registrador semântico especial: guarda o valor de retorno da função corrente. |
| **`G [expr]`** | Imprime `Y` (se não receber argumento) ou o valor de `<expr>`. É o canal de saída. |
| **`[ ... ]`** | Bloco de instruções (substitui `begin/end`). Pode ser fechado por `]` ou por `fim`. |
| **`Se <cond> [ ... ] Senao [ ... ]`** | Condicional com bloco `[ ... ]` ou `fim`. |
| **`Enquanto <cond> [ ... ]`** | Laço `while` com bloco `[ ... ]` ou `fim`. |
| **`Loop <var> in <lista> [ ... ]`** | Iteração sobre lista com `[ ... ]` de corpo. |
| **`Funcao Nome(params) [ ... ]`** | Definição de função; corpo em blocos `[ ... ]` ou `fim`. |
| **`Importar @modulo [como Alias]`** | Carrega módulo nativo ou de código (`libs/<modulo>/index.ng`, `NG-modulos/<modulo>/index.ng` ou pacote global). |

---

## 2. Instalação

### 2.1 Pré-requisitos comuns

Independente do sistema, o build do engine NG precisa de:

- **Compilador C11**: `clang` (recomendado) ou `gcc`;
- **`make`**;
- **Bibliotecas opcionais detectadas automaticamente** (o build degrada com
  `-DNG_NO_CURL`, `-DNG_NO_SQLITE`, `-DNG_NO_SODIUM` quando ausentes):
  - `libcurl` → cliente HTTP nativo (`@http`);
  - `libsqlite3` → módulo `@sqlite` (SQLite real);
  - `openssl` → TLS/HTTPS (`@https`);
  - `libsodium` → XEdDSA / Curve25519 (`@wacore`, `@cripto`);
  - `lua5.4` (+ `liblua5.4-dev`) → Core 1 do Dynamic V / backend Lua.

### 2.2 Linux

```bash
sudo apt update
sudo apt install clang make libcurl4-openssl-dev libsqlite3-dev \
     libssl-dev lua5.4 liblua5.4-dev

git clone <url-do-repositorio> ng
cd ng
make
./bin/ng executar testes/01_ola_mundo.ng
```

### 2.3 Termux (Android)

```bash
pkg update && pkg upgrade
pkg install clang make libcurl libsqlite openssl lua5.4

# dentro do projeto NG:
git clone <url-do-repositorio> ng
cd ng
make

# /storage costuma estar com noexec no Termux: usar o wrapper bash ng.sh
bash ng.sh executar testes/01_ola_mundo.ng
# ou copiar o binario para um diretorio executavel:
cp bin/ng "$PREFIX/tmp/ng" && chmod +x "$PREFIX/tmp/ng"
"$PREFIX/tmp/ng" executar testes/01_ola_mundo.ng
```

No Termux o **JIT** é desativado por padrão (`NG_JIT_DISABLE=1`) para evitar
kill do processo por `mmap RWX`; para habilitar manualmente: `NG_JIT_DISABLE=0 ng programa.ng`.

### 2.4 Docker

**Dockerfile** (na raiz do projeto):

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang make \
        libcurl4-openssl-dev libsqlite3-dev \
        libssl-dev lua5.4 liblua5.4-dev

WORKDIR /src
COPY . .
RUN make

ENTRYPOINT ["./bin/ng"]
```

**Build e execução:**

```bash
docker build -t ng .
docker run --rm -v "$PWD":/src -w /src ng executar testes/01_ola_mundo.ng
docker run --rm -i -t ng --backend=lua testes/01_ola_mundo.ng
```

### 2.5 Windows (sem PowerShell)

A NG **não requer PowerShell**: basta usar um terminal baseado em **bash**
(MSYS2 / MSYS2 mingw64 ou Git Bash com toolchain MinGW) e os comandos normais
do `make`.

```bash
# 1. Instale o MSYS2 (https://www.msys2.org) e abra o terminal "MSYS2 mingw64"
pacman -Syu
pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-make mingw-w64-x86_64-gcc

# 2. Copie/clone o projeto e compile:
cd /c/Users/seu_usuario/ng
make CC=x86_64-w64-mingw32-gcc

# 3. Execute:
./bin/ng executar testes/01_ola_mundo.ng
```

> **Nota:** o Makefile detecta automaticamente OpenSSL/libcurl/SQLite/Lua.
> Nos pacotes do MSYS2 os caminhos podem diferir; se alguma lib não for
> encontrada o build simplesmente usa os *fallbacks* (`-DNG_NO_CURL`,
> `-DNG_NO_SQLITE`, etc.), continuando o build sem elas.

---

## 3. Compilação

### 3.1 Build nativo (Linux / Termux)

```bash
make
# gera: bin/ng
```

### 3.2 Build alternativo manual

```bash
clang -Wall -Wextra -std=c11 -O2 -Iinclude src/*.c -o bin/ng -lm -lpthread -lz
```

### 3.3 Cross-compilação ARM (Termux + NDK)

```bash
apt install ndk-multilib
make -f Makefile.cross
# gera:
#   bin/ng_arm64  -> ELF 64-bit ARM aarch64 (Android)
#   bin/ng_armhf  -> ELF 32-bit ARM EABI5/hard-float (Android)
```

No cross-build o cliente HTTP libcurl é substituído pelo comando `curl`
externo (`-DNG_NO_CURL`) e o `@sqlite` é desativado (`-DNG_NO_SQLITE`).

### 3.4 Flags de otimização

```bash
make OPT_LEVEL=-O2 LTO= bench     # build alternativo de referencia
make bench                        # benchmark longo (2M iteracoes)
```

---

## 4. CLI do binário `ng`

```bash
ng                     # sem argumentos -> REPL interativo
ng versao             # versao do runtime
ng ajuda              # ajuda
ng verificar arquivo.ng   # valida sintaxe sem executar
ng compilar arquivo.ng    # valida + prepara cache N3
ng executar arquivo.ng   # executa (alias: ng arquivo.ng)
ng limpar-cache
ng dynamicv status
ng dynamicv cache
ng dynamicv limpar
ng dynamicv diagnostico
ng independencia install <pacote|URL> [-g]
ng independencia desinstalar <pacote|URL>
ng independencia list
ng des i <pacote>      # alias
ng des des <pacote>    # alias
```

Exemplos:

```bash
ng executar testes/01_ola_mundo.ng
ng executar testes/25_sqlite_basico.ng
ng verificar testes/02_variaveis.ng
ng dynamicv status
```

---

## 5. Como a NG funciona — instrução por instrução (tim-tim por tim-tim)

A documentação pede para explicar **"tim-tim por tim-tim"** como a linguagem
funciona: ou seja, o ciclo completo de vida de **uma única instrução**, do ponto
de vista do usuário, até a execução.

### 5.1 Ciclo de uma instrução em um programa (`.ng`)

Para cada instrução (statement) ou expressão do arquivo:

1. **Leitura da entrada** — o conteúdo do arquivo (`read_file`) ou da linha do
   REPL é lido por completo.
2. **Tokenização (Lexer)** — o source vira tokens:
   - ignora espaços, tabs e comentários (`#` ou `//`);
   - reconhece delimitadores (`[ ] ( ) @ . , + - * / % = == != < <= > >=`);
   - strings com aspas simples/duplas e escapes (`\n \t \r \0 \\ \" \'`);
   - números (int/float convertidos direto via `strtod`, sem alocação extra);
   - identificadores (incluindo UTF-8, a partir de byte `>= 0x80`);
   - **palavras reservadas**: `Print`, `G`, `Y`, `Se`, `Senao`, `Enquanto`,
     `Funcao`, `Importar`, `como`, `true`/`Verdadeiro`, `false`/`Falso`,
     `null`/`Nulo`, `fim`/`Fim`, `Loop`, `in` — com variantes que ignoram
     acentuação/case (ex.: `Funcao`, `Função`, `funcao`, `função`).
   - cada token carrega `line`, `column` e flag `space_before` (necessário para
     o REPL e para `[` usado como índice vs. bloco).
3. **Parsing** — o parser consome a sequência de tokens e constrói a **AST**
   (árvore sintática abstrata), com nós como `NG_AST_STMT_PRINT`,
   `NG_AST_STMT_ASSIGN`, `NG_AST_STMT_IF`, `NG_AST_STMT_WHILE`,
   `NG_AST_STMT_LOOP`, `NG_AST_STMT_FUNC`, `NG_AST_STMT_IMPORT`,
   `NG_AST_EXPR_BINARY`, `NG_AST_EXPR_ARRAY`, etc.
4. **FASE PREPARAÇÃO — Dynamic V (N3)** — antes da execução:
   - checagem do **N3 Cache** (HIT/MISS) com cache persistente em disco
     (escrita atômica) — via `ng_dynamicv_prepare_program`;
   - se **MISS**: Core 1 (worker com **Lua embutido**) gera **IR** e aplica
     otimizações semânticas/algebraicas, com **fallback seguro** para o Core 0;
   - se **HIT**: carrega AST compilada do cache.
5. **FASE EXECUÇÃO** — `ng_interp_run`:
   - tenta o **JIT nativo** (`ng_jit_run_program`) — no Android/Termux fica
     desligado por padrão (`NG_JIT_DISABLE=1`);
   - **fallback** direto para o executor interpretado em C11
     (`execute_statement` / `eval_expression`).
6. **Execução da instrução** — a AST é percorrida:
   - `Print`: avalia a expressão e imprime (via `ng_val_print`, com **Print
     inteligente**: listas imprimem elemento a elemento, tabelas `chave =
     valor`);
   - `Y = <expr>`: grava no registrador semântico da função corrente;
   - `G [expr]`: imprime `Y` ou o valor passado;
   - atribuições atualizam o escopo existente na cadeia ou criam no escopo atual;
   - `Se`/`Enquanto` avaliam condição e executam blocos;
   - `Loop` itera a lista e restaura o escopo da variável;
   - `Funcao` registra a função no escopo global;
   - `Importar` carrega o módulo (nativo via loader ou `.ng` via parser).

### 5.2 Ciclo de uma instrução no REPL (tim-tim por tim-tim)

No REPL, **cada bloco de instruções** (uma linha completa ou um bloco
multilinha) vira um mini-programa:

1. Aparece o prompt `> ` (ou `>> ` quando há bloco `[`/`(` aberto).
2. O REPL lê a linha (buffer com suporte a linhas longas via `fgets` chunks).
3. Faz **scan de profundidade** de `[`/`(` contando strings e ignorando
   comentários — se ainda houver bloco aberto, aguarda a linha seguinte com
   prompt `>> `.
4. Linhas em branco são ignoradas; `sair`/`exit` encerram.
5. Quando o bloco está fechado, o texto é **tokenizado → parseado → executado**
   como um programa.
6. O escopo **global é persistente** entre entradas (variáveis e funções
   continuam disponíveis).
7. Erros de léxico/sintaxe/execução são exibidos **sem encerrar o REPL**.

---

## 6. Referência de sintaxe

### 6.1 Palavras reservadas

| Token | Significado | Exemplo |
|---|---|---|
| `Print` | Imprime valor | `Print "Ola"` |
| `G` | Imprime `Y` ou expressão | `G Y` / `G Resposta.Status` |
| `Y` | Registrador de retorno / variável | `Y = 42` / `Y` |
| `Se` | Condicional | `Se Idade >= 18 [ ... ] Senao [ ... ]` |
| `Senao` | Contrapartida do `Se` | `Senao [ ... ]` |
| `Enquanto` | Laço `while` | `Enquanto X < 10 [ ... ]` |
| `Loop` | Iteração sobre lista | `Loop Item in Nomes [ ... ]` |
| `in` | Palavra de iteração | `Loop Item in Nomes` |
| `Funcao` | Declaração de função | `Funcao Saudacao(Nome) [ ... ]` |
| `Importar` | Importa módulo | `Importar @json` / `Importar @sqlite` |
| `como` | Alias de módulo | `Importar @json como Json` |
| `fim` | Fecha bloco sem `]` | `Funcao F() [ Print 1 ] fim` |
| `true`/`Verdadeiro` | Booleano verdadeiro | `Se Verdadeiro [ ... ]` |
| `false`/`Falso` | Booleano falso | `Se Falso [ ... ]` |
| `null`/`Nulo` | Nulo | `X = Nulo` |

### 6.2 Delimitadores e blocos

- Blocos: `[ ... ]` ou, no fim, a palavra `fim`:

```ng
Se Idade >= 18 [
    Print "Maior"
] Senao [
    Print "Menor"
]

Enquanto Contador <= 3 [
    Print Contador
    Contador = Contador + 1
]

Funcao Inc() [
    Contador = Contador + 1
]
Inc()
Inc()
```

- `[` também serve de **índice** em valores: `Lista[0]`, `Objeto["campo"]`,
  `Texto[1]`. Em condições (`Se`/`Enquanto`/`Loop`) o parser desativa o modo
  índice automaticamente para não confundir `[` de bloco com índice.

### 6.3 Expressões

- Literais: números (`42`, `5.5`), strings (`"texto"`, `'texto'` com escapes),
  booleanos, `null`.
- Listas: `[1, 2, 3]`, `[]`.
- Acesso: `tabela.campo`, `tabela["campo"]`, `lista[0]`, `texto[1]`.
- Chamadas: `Funcao(args)`, `Modulo.Funcao(args)`, `obj.metodo(args)`.
- Operadores: `+ - * / %`, `== != < <= > >=`, concatenação de strings via `+`.

```ng
Resultado = 21 * 2 + 1          # 43
Texto = "Ola" + " Mundo"         # concatenação
Se Lista[0] == 10 [ Print "ok" ] Senao [ Print "erro" ]
```

### 6.4 Comentários

```ng
# comentario de linha
// comentario de linha (aceito)
Print "Ola"   # comentario
```

---

## 7. Tipos e valores

| Tipo | Valores | Observações |
|---|---|---|
| `null` / `Nulo` | nulo | Falsy. |
| `booleano` | `true`/`false`/`Verdadeiro`/`Falso` | Falsy/Truthy. |
| `numero` | `double` (inteiro ou decimal) | Operações aritméticas. Divisão por zero → `0` (com diagnóstico se `NG_DIAG=1`). |
| `string` | texto UTF-8 | Concatenação com `+`. Índice acessa caractere. |
| `tabela` | registro/objeto | Hash table com contagem; usado para JSON, resultados de módulo, etc. |
| `funcao` | função NG | Corpo em AST. |
| `funcao nativa` | função de módulo | Implementada em C (`loader`). |

**Truthy/Falsy** (usados em `Se`, `Enquanto`, `Loop` condições):

- `null` → falso; `false` → falso; `0` → falso;
- string vazia → falso; tabela vazia (`count == 0`) → falso;
- `true`, número != 0, string não vazia, função → verdadeiro.

---

## 8. Variáveis, escopos e funções

- **Escopos**: a NG mantém uma **cadeia de escopos** (`NGEnv` com tabela hash e
  ponteiro para o pai). O escopo global é a raiz; cada chamada de função cria
  um **novo escopo filho**.
- **Atribuição**: `Nome = <expr>` atualiza a variável na cadeia de escopos onde
  ela existe (ex.: variável global acessada dentro da função); caso não exista,
  cria no escopo atual.
- **Escopo de bloco**: blocos não criam escopo novo; variáveis ficam no escopo
  do bloco atual (global na maioria dos casos).
- **`Loop`**: a variável do loop existe apenas durante a iteração e o valor
  anterior é **restaurado** ao final (inclusive em loops aninhados e com erro).
- **`Y` (retorno semântico)**: o valor de retorno da função é o último gravado em
  `Y`. A cada chamada, `Y` é resetado para `null` (retorno deterministico).
- **Funções sem `Y`**: retornam `null`.

```ng
Contador = 0
Funcao Inc() [
    Contador = Contador + 1
]
Inc()
Inc()
Print Contador   # 2
```

---

## 9. Módulos nativos

O loader nativo registra módulos com `@` interno (o `@` é removido para achar o
registro). Módulos registrados incluem:

- **Sistema**: `fs`, `path`, `os`, `env`, `process`, `thread`, `logger`, `sys`.
- **Dados**: `sqlite`, `sqlite3`, `json`, `csv`, `xml`, `yaml`, `toml`, `ini`,
  `cache`, `collections`.
- **Internet**: `http`, `https`, `websocket`, `dns`, `proxy`, `reverse_proxy`,
  `download`, `url`.
- **Programação / utilitários**: `math`, `crypto`, `hash`, `regex`, `time`,
  `date`, `base64`, `hex`, `zlib`, `zip`, `tar`.
- **Dev / integração**: `format`, `print`, `info`, `diag`, `help`, `debug`,
  `test`, `package`, `systeminfo`, `memory`, `interop`, `qr`, `qrcode`,
  `cripto`, `ws`, `zap`, `wacore`, `MyLanguage`, `N3-cache`.

Catálogo completo com status (`real`, `parcial`, `planejado`) via
`Info.Modulos()` / `Info.Tudo()`.

---

## 10. Bibliotecas e superfícies de API

### 10.1 JSON (`@json`)

```ng
Importar @json
Importar @fs

Dados = JSON.Parse("{\"nome\":\"Miguel\",\"idade\":20}")
Print Dados.nome
Serializado = JSON.Stringify(Dados)

Valido = JSON.Validar("{\"ok\":true}")
Tipo = JSON.Tipo("[1,2,3]")

JSON.Definir(Dados[0], "cidade", "São Paulo")
JSON.Obter(Dados[0], "cidade")
JSON.Chaves(Dados[0])
JSON.Remover(Dados[0], "idade")
JSON.Formatar(Dados)

Gravou = JSON.Gravar("saida.json", Dados)
Reidos = JSON.Ler("saida.json")
Tamanho = JSON.Tamanho(Dados)
JSON.Copiar(Dados)
```

### 10.2 SQLite real (`@sqlite`)

```ng
Importar @sqlite
Importar @json

Banco = SQLite(":memory:")
Banco.CriarTabela("CREATE TABLE usuarios (id INTEGER PRIMARY KEY, nome TEXT, idade INTEGER)")
Banco.Inserir("usuarios", JSON.Parse("{\"nome\":\"Ana\",\"idade\":30}"))
Banco.Consultar("SELECT * FROM usuarios WHERE idade > ?", [18])
Banco.Executar("SELECT COUNT(*) AS total FROM usuarios", [])
Banco.Atualizar("usuarios", JSON.Parse("{\"nome\":\"Ana Maria\"}"), "id = ?", [1])
Banco.Excluir("usuarios", "id = ?", [1])
Banco.ListarTabelas()
Banco.Estrutura("usuarios")
Banco.Colunas("usuarios")
Banco.CriarIndice("CREATE INDEX idx ON tabela(coluna)")
Banco.Iniciar()
Banco.Confirmar()
Banco.Desfazer()
Banco.UltimoErro()
Banco.Versao()
Banco.Fechar()
Banco.Params("listar")                       # comandos de biblioteca
Banco.Params("criar tabela", "CREATE TABLE notas (id INTEGER PRIMARY KEY)")
Banco.Params("inserir usuario", "usuarios", JSON.Parse("{\"id\":1}"))
Banco.Params("consultar usuarios", "SELECT nome FROM usuarios", [])
```

### 10.3 Formatação / impressão avançada (`@format`)

- `Format.Formatar(valor, modo)` — **64 modos reais**: `texto`, `tipo`, `tamanho`,
  `numero`, `inteiro`, `hex`, `binario`, `json`, `json-indentado`, `csv`,
  `markdown`, `xml`, `tabela`, `tabela-compacta`, `tabela-detalhada`,
  `tabela-cabecalho`, `tabela-linhas`, `bordas`, `sem-bordas`, `indentado`,
  `tabulado`, `separador`, `prefixo`, `sufixo`, `titulo`, `cabecalho`,
  `rodape`, alinhamentos, `largura20`, `quebra`, `enumerado`, `marcadores`,
  `progresso`, `status`, `aviso`, `erro`, `sucesso`, `info`, `debug`,
  `destaque`, `cores`, `sem-cores`, `resumo`, `estrutura`, `metadados`,
  `paginado`, `limite`, `truncado`, `estatisticas`, `tempo`, e modos de info.
- `Format.Modos()` — lista de modos disponíveis.
- `Format.Imprimir(valor, modo)`.

### 10.4 Informações e diagnóstico (`@info`, `@diag`, `@help`)

```ng
Importar @info
Importar @diag
Importar @help

Print Info.Versao()
Print Info.Linguagem()
Print Info.Runtime()
Print Info.Compilador()
Print Info.Arquitetura()
Print Info.Sistema()
Print Info.Diretorio()
Print Info.Memoria()
Print Info.Configuracao()
Print Info.Recursos()
Print Info.Modulos()
Print Info.Tudo()

Print Diag.Condicao(false)
Print Diag.Colecao([])
Print Diag.Variavel("variavel_inexistente")
Print Diag.SQL()

Print Ajuda.Ajuda()
```

Com `NG_DIAG=1` a NG emite automaticamente dicas (`[NG-407]`) para condições
falsas, listas vazias e divisão por zero, **sem alterar a lógica**.

### 10.5 HTTP (`@http`)

- **Cliente**: `HTTP.Get(url)`, `HTTP.Post(url, dados)`, `HTTP.Put(...)`,
  `HTTP.Patch(...)`, `HTTP.Delete(...)`, `HTTP.Requisicao(...)` — respostas
  com `.Status`, `.Ok`, `.JSON`, `.HTML`, `.Texto`, etc.
- **Servidor**: `Servidor = HTTP()`, rotas com `Servidor.Get/Post/Put/Patch/
  Delete/Use(...)`, handlers `Função(req, res) [ ... ]`, `Servidor.Listen(porta)`.

```ng
Importar @http

Servidor = HTTP()
Servidor.Get("/", Função(req, res) [
    res.HTML("<h1>NG funcionando!</h1>")
])
Servidor.Post("/usuarios", Função(req, res) [
    Dados = req.JSON()
    res.Status(201).JSON(Dados)
])
Servidor.Listen(8080)
```

### 10.6 Cripto / QR / WS / Zap / WACore / Interop

- `@qr`: `QR.Terminal(texto, tamanho)`, `QR.SalvarPNG(caminho, nome, escala) == true`.
- `@crypto` / `@cripto`: SHA-256, XEdDSA, Curve25519.
- `@ws`: WebSocket.
- `@zap`, `@wacore`: superfícies de WhatsApp / núcleo Wide.
- `@interop`: execução de código em Lua/Python/Go e conversão de valores via JSON:

```ng
Importar @interop

Print Interop.Python("print('ola do python')")
Print Interop.Lua("print('ola da lua')")
Dobro = Interop.Trocar("python", "import json,os\nv=json.loads(os.environ['NG_INPUT'])\nprint(json.dumps({'valor': v*2}))", 21)
Print Dobro.valor
```

---

## 11. Códigos de erro

| Código | Erro |
|---|---|
| `NG-001` | Erro desconhecido |
| `NG-002` | Arquivo não encontrado |
| `NG-003` | Permissão |
| `NG-101` | Token inválido |
| `NG-103` | Bloco não fechado |
| `NG-104` | Bloco inesperado |
| `NG-106` | Erro de sintaxe |
| `NG-107` | Variável não definida |
| `NG-108` | Tipo inválido no Loop / mismatch |
| `NG-109` | Função não encontrada |
| `NG-110` | Argumentos inválidos |
| `NG-201` | Cache inválido |
| `NG-301` | Módulo não encontrado |
| `NG-504` | Conflito de módulo |
| `NG-501` | Falha no subinterpretador |
| `NG-401`–`NG-405` | Erros SQLite (conexão, query, parâmetro, vazio, módulo) |
| `NG-406` | Erro JSON |
| `NG-407` | Diagnóstico |

Formato padrão:

```text
[NG-106] Erro de Sintaxe

Arquivo: testes/x.ng
Linha: 12
Coluna: 3

Descrição:
...

Sugestão:
...
```

---

## 12. REPL interativo

```text
NG v0.3.0-alpha
REPL interativo. Digite 'sair' ou Ctrl+D para encerrar.
> Nome = "Miguel"
> Funcao Saudacao(N) [
>> Print N
>> ]
> Saudacao(Nome)
Miguel
> sair
```

Comportamento:

- escopo global persistente entre linhas;
- blocos multilinhas com prompt secundário `>> `;
- `sair`/`exit`/`Ctrl+D` encerram;
- erros não encerram o REPL.

---

## 13. Dynamic V, N3 Cache, JIT e Backend Lua

### 13.1 Dynamic V — Dual Core

- **Core 0** (C11): executor / runtime nativo e fallback.
- **Core 1** (Lua embutido): worker de otimização semântica e geração de IR.
- **L2**: barramento com fila de tarefas e sincronização C11.
- **N3 Cache**: cache persistente de unidades compiladas em disco (escrita
  atômica).

Comandos:

```bash
ng dynamicv status
ng dynamicv cache
ng dynamicv limpar
ng dynamicv diagnostico
```

### 13.2 Backends

- `--backend=c` (padrão): fluxo Dynamic V / N3.
- `--backend=lua`: transpila o source NG → Lua em memória e executa no runtime
  Lua (via `ng_transpile_to_lua` + `ng_net_libs_exec_source`).
- Variável de ambiente: `NG_BACKEND=c|lua`.

### 13.3 Subinterpretador

- `ng executar arquivo.ng --subinterpreter` / `--no-subinterpreter` (padrão).

---

## 14. Testes

```bash
make test            # bateria principal
make test-all        # suíte completa
make test-loop       # testes de Loop/listas
make test-sqlite     # testes de SQLite
make test-json       # testes de JSON
make test-print      # testes de Print/Info/Diag
make bench           # benchmark (2M iterações)
```

Scripts auxiliares em `testes/*.sh`.

---

## 15. Desempenho e otimizações

Build otimizado: `-Os -flto`, seções separadas, `--gc-sections`, strip,
remoção de tabelas unwind.

```text
Tamanho do executável: 194.288 B -> 132.808 B  (-31,6%)
Benchmark 3 execuções (2M iterações): 2,066 s -> 1,792 s  (~-13%)
RSS reportado: ~5.000 KiB -> ~4.780 KiB
```

---

## 16. Exemplos completos

### 16.1 Olá mundo

```ng
Print "Ola, mundo! Bem-vindo a NG Language."
```

### 16.2 Variáveis

```ng
Nome = "Miguel"
Idade = 20
Velocidade = 5.5
Print Nome
Print Idade
Print Velocidade
```

### 16.3 Condicionais e laços

```ng
Idade = 20
Se Idade >= 18 [
    Print "Maior de idade"
] Senao [
    Print "Menor de idade"
]

Contador = 1
Enquanto Contador <= 3 [
    Print Contador
    Contador = Contador + 1
]
```

### 16.4 Listas e Loop

```ng
Nomes = ["Carlos", "Renato", "Ângela", "Maria"]
Loop Nome in Nomes [
    Print Nome
]
Print Nomes        # imprime um elemento por linha
Vazia = []
```

### 16.5 Função com retorno via `Y`

```ng
Função Saudacao(Nome) [
    Print "Ola"
    Print Nome
    Y = Nome
]

Saudacao("Miguel")
```

### 16.6 JSON

```ng
Importar @json
Objeto = JSON.Parse("{\"nome\":\"Miguel\",\"idade\":20,\"ativo\":true}")
Print Objeto.nome
Print Objeto.ativo
Serializado = JSON.Stringify(Objeto)
Print Serializado
```

---

## 17. Perguntas frequentes

- **Como executar sem compilar?** Use o REPL: `ng` (sem argumentos).
- **Como verificar um programa?** `ng verificar arquivo.ng`.
- **Onde fica o cache?** Diretório N3 (ver `ng dynamicv cache`).
- **Erro de bloco não fechado?** Use `]` ou `fim`.
- **Variável não definida?** Atribua antes de usar (`Nome = "x"`).
- **Como instalar dependência?** `ng independencia install nome -g`.
- **Como mudar backend?** `ng executar arquivo.ng --backend=lua` ou `NG_BACKEND=lua`.

---

*Docs atualizadas para o engine **NG v0.3.0-alpha** — fonte: `src/`, `include/`,
`testes/`, `Makefile` do repositório.*
