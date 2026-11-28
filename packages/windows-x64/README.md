# NG (Next Generation) Language

> 📘 **Documentação completa da linguagem**: [`DOCUMENTACAO.md`](DOCUMENTACAO.md)

Linguagem de programação independente, com identidade sintática própria, interpretador nativo, AST modular e CLI construído em **C puro (padrão ISO C11)**.

---

## 1. Estrutura do Projeto

```
oi/
├── Makefile                # Build automatizado para Linux/Android/Windows
├── include/                # Cabeçalhos da arquitetura C11
│   ├── ng.h
│   ├── ng_lexer.h          # Análise léxica e tokens próprios
│   ├── ng_parser.h         # Análise sintática com blocos [...]
│   ├── ng_ast.h            # Árvore Sintática Abstrata (AST)
│   ├── ng_value.h          # Sistema unificado de tipos e valores
│   ├── ng_runtime.h        # Tabelas de símbolos e escopos
│   ├── ng_interpreter.h    # Avaliador e runtime nativo
│   ├── ng_error.h          # Códigos de erro NG-XXX
│   ├── ng_module.h         # Resolução em NG-modulos/
│   └── ng_n3cache.h        # Cache N3 e integridade
├── src/                    # Implementações em C11
│   ├── main.c              # CLI "ng"
│   ├── lexer.c
│   ├── parser.c
│   ├── ast.c
│   ├── value.c
│   ├── runtime.c
│   ├── interpreter.c
│   ├── repl.c
│   ├── ng_process.c
│   ├── ng_dependency.c
│   └── ng_interop.c
├── NG-modulos/             # Módulos centrais da linguagem
└── testes/                 # Bateria de programas .ng
    ├── 01_ola_mundo.ng
    ├── 02_variaveis.ng
    ├── 03_g_y.ng
    ├── 04_condicoes.ng
    └── 05_funcoes.ng
```

---

## 2. Como Compilar no Termux / Linux

Para compilar o binário `ng`:

```bash
make
```

Ou diretamente com o compilador C:

```bash
clang -Wall -Wextra -std=c11 -O2 -Iinclude src/*.c -o bin/ng
```

---

## 3. Comandos do CLI

```bash
# Iniciar o REPL interativo (sem argumentos)
./bin/ng

# Exibir versão e ajuda
./bin/ng versao
./bin/ng ajuda

# Verificar sintaxe sem executar
./bin/ng verificar testes/01_ola_mundo.ng

# Executar programas NG
./bin/ng executar testes/01_ola_mundo.ng
./bin/ng executar testes/03_g_y.ng
./bin/ng executar testes/04_condicoes.ng
./bin/ng executar testes/05_funcoes.ng

# Executar bateria de testes completa
make test
```

### REPL interativo

Executar `ng` sem argumentos inicia o REPL nativo (equivalente ao `lua` do
Lua). Em mounts `/storage` com `noexec`, use o wrapper do projeto:

```bash
bash ng.sh          # inicia o REPL NG
bash ng.sh executar testes/01_ola_mundo.ng
```

Ou copie o binário para um diretório executável:

```bash
cp bin/ng "$PREFIX/tmp/ng" && chmod +x "$PREFIX/tmp/ng"
"$PREFIX/tmp/ng"
```



- O escopo global é persistente: variáveis e funções definidas continuam
  disponíveis nas linhas seguintes.
- Blocos multilinhas são lidos automaticamente enquanto houver `[` ou `(`
  abertos, usando o prompt secundário `>> `.
- `sair`, `exit` ou `Ctrl+D` encerram a sessão.
- Erros de léxico, sintaxe ou execução são exibidos sem encerrar o REPL.

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

### Independência — gerenciador de dependências

```bash
# Instalar por nome, URL Git ou URL comum
ng independência install meu-pacote
ng des i https://github.com/usuario/repositorio.git
ng des i https://exemplo.com/pacote.ng

# Instalação global (PREFIX/lib/ng/modulos) com -g
ng independência install meu-pacote -g

# Remover e listar
ng independência desinstalar meu-pacote
ng des des meu-pacote
ng independência list
ng des list
```

Todas as operações atualizam `Independência.txt.ng` no diretório atual, com
nome, origem, URL, versão, escopo (local/global), status e caminho instalado.
Pacotes globais passam a ser encontrados por `Importar @nome`.

### Interoperabilidade NG ↔ Lua, Python e Go

O módulo nativo `@interop` executa código nas três linguagens e troca valores
usando JSON (`NG_INPUT` → processo → JSON de volta para a NG):

```text
Importar @interop

Print Interop.Python("print('ola do python')")
Print Interop.Lua("print('ola da lua')")

Dobro = Interop.Trocar("python", "import json,os\nv=json.loads(os.environ['NG_INPUT'])\nprint(json.dumps({'valor': v*2}))", 21)
Print Dobro.valor
```

Também existem `Interop.Executar(lang, code)`, `Interop.ParaJSON(valor)`,
`Interop.ParaNG(json)` e o módulo legado `MyLanguage.Go(code)`.

### Loop, listas e Print inteligente

```text
Nomes = ["Carlos", "Renato", "Ângela", "Maria"]

Loop Nome in Nomes [
    Print Nome
]

Print Nomes        # imprime um elemento por linha
Vazia = []         # lista vazia (Loop nao executa o bloco)
```

- `Loop <variavel> in <lista> [ ... ]` percorre automaticamente todos os
  elementos; a variável existe apenas durante o loop e seu valor anterior é
  restaurado ao final (escopo correto, inclusive em loops aninhados).
- Listas aceitam strings, números, UTF-8 e podem ser vazias.
- `Print` de uma lista mostra cada elemento em uma linha; `Print` de uma tabela
  comum mostra `chave = valor` linha a linha.

Testes: `bash testes/test_loop.sh bin/ng` (ou `make test-loop`).

### Banco de dados SQLite real (`@sqlite`)

Usa a biblioteca `sqlite3` de verdade (NÃO é simulação). Suporta `.db`,
`.sqlite`, `.sqlite3` e `:memory:`.

```text
Importar @sqlite
Importar @json

Banco = SQLite("usuarios.db")
Banco.CriarTabela("CREATE TABLE usuarios (id INTEGER PRIMARY KEY, nome TEXT, idade INTEGER)")
Banco.Inserir("usuarios", JSON.Parse("{\"nome\":\"Ana\",\"idade\":30}"))

Usuarios = Banco.Consultar("SELECT * FROM usuarios WHERE idade > ?", [18])
Print Usuarios
Loop U in Usuarios [ Print U.nome ]
```

Operações: `Executar`, `Consultar`, `Inserir`, `Atualizar`, `Excluir`,
`ListarTabelas`, `Estrutura`, `Colunas`, `CriarIndice`, `Iniciar`, `Confirmar`,
`Desfazer`, `Fechar`, `UltimoErro`, `Versao`, `Ajuda`.

Sistema de parâmetros (comandos de biblioteca):

```text
Print Banco.Params("listar")
Print Banco.Params("criar tabela", "CREATE TABLE notas (id INTEGER PRIMARY KEY)")
Print Banco.Params("inserir usuario", "usuarios", JSON.Parse("{\"id\":1}"))
Print Banco.Params("consultar usuarios", "SELECT nome FROM usuarios", [])
Print list("listar")
```

`list(...)` usa a última conexão aberta. Parâmetros desconhecidos retornam
ajuda em vez de falhar silenciosamente. Consultas usam `sqlite3_prepare_v2`
com parâmetros ligados (proteção contra SQL injection). `Excluir` sem `WHERE`
exige confirmação explícita.

SQL suportado é o próprio SQLite: JOIN, LEFT JOIN, GROUP BY, HAVING, ORDER BY,
LIMIT/OFFSET, subconsultas, COUNT/SUM/AVG/MIN/MAX, DISTINCT, LIKE/IN/BETWEEN,
chaves primárias/estrangeiras, índices e transações.

Testes: `bash testes/test_sqlite.sh bin/ng` (ou `make test-sqlite`).

**MySQL:** não implementado. Não há `libmysqlclient` neste ambiente; a
arquitetura já separa drivers por módulo (`@sqlite` ≠ `@mysql`) e o catálogo
`Info.Modulos()` marca `@mysql` como `planejado`.

### JSON completo (`@json`)

`Ler`, `Gravar`, `Validar`, `Parse`, `Stringify`, `Obter`, `Definir`, `Remover`,
`Chaves`, `Tipo`, `Formatar`, `Tamanho`, `Copiar`. Resultados são coleções NG
compatíveis com `Loop` e `Print`.

Testes: `bash testes/test_json.sh bin/ng` (ou `make test-json`).

### Print avançado, Info e diagnósticos

`@format` expõe `Format.Formatar(valor, modo)`, `Format.Imprimir` e
`Format.Modos()`. Existem **64 modos reais**, entre eles: `texto`, `tipo`,
`tamanho`, `numero`, `hex`, `binario`, `json`, `json-indentado`, `csv`,
`markdown`, `xml`, `tabela`, `tabela-compacta`, `tabela-detalhada`,
`tabela-linhas`, `sem-bordas`, `indentado`, `separador`, `prefixo`, `sufixo`,
`titulo`, `cabecalho`, `rodape`, alinhamentos, `largura20`, `quebra`,
`enumerado`, `marcadores`, `progresso`, `status`, `aviso`, `erro`, `sucesso`,
`info`, `debug`, `destaque`, `cores`, `sem-cores`, `resumo`, `estrutura`,
`metadados`, `limite`, `truncado`, `estatisticas`, `tempo` e modos de info.

`@info` fornece `Versao()`, `Linguagem()`, `Runtime()`, `Compilador()`,
`Arquitetura()`, `Sistema()`, `Diretorio()`, `Memoria()`, `Configuracao()`,
`Recursos()`, `Modulos()` e `Tudo()` — todos com dados detectados em runtime.

`@diag` fornece `Condicao()`, `Colecao()`, `Variavel()` e `SQL()`. Com
`NG_DIAG=1`, condições falsas, listas vazias e divisão por zero também emitem
dica automaticamente, sem alterar a lógica do programa.

Testes: `bash testes/test_print.sh bin/ng` (ou `make test-print`).
Suíte completa: `make test-all`.

## 4. Cross-compilação ARM (32 e 64 bits)

```bash
# Requer o pacote Termux ndk-multilib (bionic libc armv7a + aarch64)
apt install ndk-multilib

make -f Makefile.cross
# gera:
#   bin/ng_arm64  -> ELF 64-bit ARM aarch64 (Android)
#   bin/ng_armhf  -> ELF 32-bit ARM EABI5/hard-float (Android)
```

O alvo `arm64` foi executado com sucesso neste ambiente. O binário `armhf`
foi gerado e validado com `file`; não executa neste dispositivo porque o
runtime Android instalado é somente 64-bit (`error: 32-bit instead of 64-bit`).

Nas compilações cruzadas o cliente HTTP libcurl é substituído pelo comando
`curl` externo (`-DNG_NO_CURL`) e o `@sqlite` é marcado como indisponível
(`-DNG_NO_SQLITE`), pois o NDK não fornece libcurl/libsqlite3 para ARM. O build
nativo no Termux (`make`) usa SQLite real e libcurl.

## 5. Otimização radical da estrutura original

Nenhuma funcionalidade foi removida. As mudanças foram:

- Build: `-Os -flto`, seções separadas, `--gc-sections`, strip e remoção de
  tabelas de unwind. `CC=clang` é usado explicitamente (o wrapper `cc` deste
  ambiente gerava um binário com comportamento diferente com LTO).
- Lexer: literais numéricos não alocam mais texto (`strtod` direto do source).
- Parser: texto dos tokens é transferido para a AST sem `strdup` e liberado
  após o parse (`ng_parser_cleanup()`), eliminando um vazamento por token.
- Runtime: `ng_env_new()` deixou de alocar um `NGTable` separado só para copiá-lo.
- CLI: registro de módulos nativos é feito apenas quando há execução/REPL,
  acelerando `versao`, `ajuda` e `limpar-cache`.
- Avisos de redefinição de `_GNU_SOURCE` eliminados.

Benchmarks reais neste ambiente (valores variam com carga/thermal):

```text
Tamanho do executável: 194.288 B -> 132.808 B  (-31,6%)
Benchmark 3 execuções (2M iterações): 2,066 s -> 1,792 s  (~-13%)
RSS reportado: ~5.000 KiB -> ~4.780 KiB
```

Comandos:

```bash
make          # build nativo otimizado
make bench    # benchmark longo (2M iterações)
make test-all # suíte completa
```

Para comparar com o build antigo:

```bash
make OPT_LEVEL=-O2 LTO= bench
```
