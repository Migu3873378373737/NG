# NG — Port para Windows x64

> Portabilidade completa da linguagem **NG (Next Generation)** para **Windows x64**,
> mantendo a arquitetura original (lexer → parser → AST → runtime → interpretador →
> módulos nativos) e todas as funcionalidades que dependem apenas do runtime.

O ponto de entrada do port é [`Makefile.windows`](Makefile.windows) e a camada de
compatibilidade fica em [`compat/windows/`](compat/windows/).

---

## 1. Requisitos de execução

| Item | Valor |
|---|---|
| Sistema | Windows 7 SP1 ou superior, **x64** |
| Arquitetura | PE32+ (x86-64), console (CUI) |
| DLLs | `WS2_32.dll`, `PSAPI.DLL`, `bcrypt.dll`, `KERNEL32.dll` + UCRT (`api-ms-win-crt-*.dll`) — todas de sistema, **nenhuma instalação extra** |
| Arquivos | `ng.exe` + pasta `libs/` no mesmo diretório de trabalho |

> A UCRT faz parte do Windows 10+. Em Windows 7/8.1 instale o
> [Universal CRT](https://support.microsoft.com/topic/update-for-universal-crt-in-windows-8.1-and-windows-server-2012-r2-2cc338ce-8298-df0b-e3e4-6c245195604a)
> antes de usar o `ng.exe` (ou use a atualização KB2999226).

## 2. Como usar

```bat
:: REPL interativo (sem argumentos)
ng.exe

:: Executar um programa .ng
ng.exe executar caminho\para\programa.ng

:: Atalho (equivalente ao comando acima)
ng.exe caminho\para\programa.ng

:: Ajuda / versão
ng.exe ajuda
```

No REPL digite `sair` ou `Ctrl+D` para encerrar.

**Importante:** o runtime resolve os módulos de biblioteca (`Importar @fs`,
`Importar @math`, …) na pasta `libs/` relativa ao **diretório de trabalho**.
Execute o `ng.exe` a partir da raiz do bundle (onde está a pasta `libs/`), ou
copie `libs/` para o diretório de onde você roda os scripts.

```bat
cd dist\windows-x64
ng.exe executar testes\01_ola_mundo.ng
```

## 3. Bundle `dist/windows-x64/`

```
dist/windows-x64/
├── ng.exe              # runtime NG para Windows x64 (PE32+ console)
├── libs/               # 29 bibliotecas padrão (.ng) — exigidas em runtime
│   ├── fs/  math/  json/  http/  cripto/  qr/  ws/  ...
├── testes/             # bateria de 43 programas .ng de exemplo/teste
├── README.md           # documentação geral da linguagem
├── DOCUMENTACAO.md     # documentação completa da linguagem
└── README-WINDOWS.md   # este arquivo
```

## 4. Compilar (cross-compile a partir de Termux/Linux)

O port é compilado com o toolchain **llvm-mingw** (`x86_64-w64-mingw32-clang`),
não exigindo Windows para gerar o binário:

```bash
# 1) build Windows
make -f Makefile.windows
#    → bin/windows/ng.exe

# 2) verificação sintática rápida de todos os fontes (0 erros esperado)
bash $PREFIX/bin/ng_win_errcheck.sh

# 3) build Linux continua funcionando (fontes idênticos, guards #ifdef _WIN32)
make
make test        # 43 testes — "Todos os testes concluidos com sucesso!"
```

Flags usadas no build Windows (ver `Makefile.windows`):

```
-D_GNU_SOURCE -DNG_NO_CURL -DNG_NO_SQLITE -DNG_NO_ZLIB -D_WIN32_WINNT=0x0600
-lm -lws2_32 -lpsapi -lbcrypt
```

### Estrutura da porta

```
compat/windows/            # camada de compatibilidade (só incluída sob #ifdef _WIN32)
├── posix_compat.h         # clock_gettime/nanosleep/setenv/unsetenv (Win32 real)
├── unistd_compat.h        # unistd + inclui posix_compat
├── dir_compat.h           # mkdir(path,mode) → _mkdir(path)
├── net_compat.h           # Winsock2 + WSAStartup idempotente (ng_win_net_init)
├── poll_compat.h          # poll() → WSAPoll
├── unistd_ext_compat.h    # close/read/write/unlink → _close/_read/_write/_unlink
├── string_compat.h        # strcasestr etc.
├── sys_compat.h           # utilitários de sistema
├── rusage_compat.h        # getrusage
└── pthread.h              # threads (winpthreads do toolchain)
Makefile.windows           # build Windows separado (não toca no Makefile Linux)
```

Regras da porta:

* Toda mudança de comportamento está atrás de `#ifdef _WIN32` / `#else` — os
  fontes continuam idênticos para Linux/Android e `make test` continua verde.
* APIs POSIX ausentes no MinGW foram reimplementadas em `compat/windows/`
  (nada foi removido ou substituído por stub de fachada).
* CSPRNG no Windows: `BCryptGenRandom` (bcrypt.dll) no lugar de `/dev/urandom`.
* Sockets: `closesocket`, `ioctlsocket(FIONBIO)`, `WSAPoll`, erros via
  `WSAGetLastError()` (`WSAEWOULDBLOCK`/`WSAEINTR`), `MSG_NOSIGNAL` ausente → 0.

## 5. Módulos nativos

### Totalmente funcionais no Windows

| Módulo | Descrição |
|---|---|
| `Cripto` | SHA-256/512, HMAC, HKDF, AES-GCM/CBC, X25519, aleatoriedade |
| `QR` | matriz, terminal, PNG (deflate *stored* sem dependências) |
| `Zap` | protocolo WhatsApp: Noise, protobuf/wabinary, **inflate RFC1951 embutido** |
| `WS` | cliente WebSocket (`ws://`), frames, máscara, handshake |
| `Wacore` | cliente WhatsApp completo (estado, pareamento, envio) |
| `HTTP` | servidor + cliente HTTP por sockets (`http://`, `Get/Post/Put/Patch/Delete/Requisicao`) |
| `JSON` | parse/geração JSON |
| `Process` / `Interop` | processos, interop Python/Lua/Go (temp em `%TEMP%`) |
| `Print` / `fs` / `math` / `time` / `date` / `env` / `regex` / `csv` / `url` / `base64` / `hex` / `logger` / … | biblioteca padrão (29 libs) |
| Dynamic V / L2 / N3 / JIT | dual-core, fila L2, cache N3 em disco, JIT |

### Desativados neste build (flags em `Makefile.windows`)

| Flag | Motivo | Efeito observável |
|---|---|---|
| `-DNG_NO_SQLITE` | sem libsqlite3 no toolchain MinGW | `SQLite.Disponivel == false`; `SQLite.Abrir()` retorna erro amigável (módulo carrega sem quebrar) |
| `-DNG_NO_CURL` | sem libcurl no toolchain MinGW | cliente `ng_http_client` devolve `NULL`; o `@http` de sockets (`HTTP.Get`, …) **continua funcionando** |
| `-DNG_NO_ZLIB` | sem libz no toolchain MinGW | `ng_zap.c` usa um **inflate RFC1951 completo embutido** (stored/fixed/dynamic Huffman) — o tráfego comprimido do Zap continua real |
| sem `NG_HAS_OPENSSL` | OpenSSL MinGW indisponível neste ambiente | `https://` e `wss://` retornam erro explícito; `http://` e `ws://` funcionam |

> Nenhum módulo foi removido: todos continuam registrados e respondem com
> erro claro quando a dependência de sistema não existe. Habilitar
> SQLite/curl/OpenSSL no Windows = fornecer as bibliotecas MinGW e remover a
> respectiva flag `NG_NO_*` no `Makefile.windows`.

## 6. Testes

A bateria em `testes/` (43 programas `.ng`) roda no Linux com `make test` sobre
os **mesmos fontes** que geram o `ng.exe`. No Windows:

```bat
cd dist\windows-x64
for %f in (testes\*.ng) do @ng.exe executar "%f"
```

Smoke test dos módulos de rede/cripto (executado no Linux na build da porta):

```
Cripto.Sha256("616263") → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
Cripto.X25519Publica(...) → 32 bytes
Zap.Iniciar() → 1 ; Zap.QuadroInicial(1) → hex do ClientHello real
WS.Erro() → "" ; Wacore.Estado() → "desconectado"
QR.Matriz("NG) → matriz 21×21 válida
REPL: 1+2, Print, sair → OK
```

## 7. Limitações conhecidas

1. **Validação de execução:** o ambiente de porta (Termux/Android) não tem
   `wine`, então o `ng.exe` foi validado por compilação + ligação limpas
   (PE32+ conferido, imports inspecionados) e pelos testes do binário Linux
   construído a partir dos **idênticos fontes**. A execução em Windows real
   é o passo de aceitação final.
2. **Sem `https://` / `wss://`** sem OpenSSL (ver §5).
3. **Sem SQLite nativo** sem a libsqlite3 MinGW (ver §5).
4. **Caminhos:** usamos `/` nas APIs C (o Win32 aceita `/` na maioria dos
   casos); symlinks não são suportados no fluxo de dependências
   (`lstat` → `stat` sob `_WIN32`).
5. **Temp:** arquivos temporários do interop usam `GetTempPathA()` (`%TEMP%`).

## 8. Solução de problemas

| Sintoma | Causa / solução |
|---|---|
| `Modulo Nao Encontrado` (NG-XXX) | a pasta `libs/` não está visível a partir do CWD — execute o `ng.exe` de onde ela está (§2) |
| `HTTPS requer suporte a OpenSSL` | esperado neste build — use `http://` ou recompile com OpenSSL MinGW |
| `SQLite... indisponível` | esperado neste build — ver §5 |
| `api-ms-win-crt-*.dll não encontrada` | instale a UCRT (Windows 7/8.1, ver §1) |
| erro de link ao recompilar | rode `bash $PREFIX/bin/ng_win_errcheck.sh` para localizar o arquivo |
