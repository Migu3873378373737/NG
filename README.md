NG Language Engine

NG é uma linguagem de programação experimental e um engine desenvolvido em C, com interpretador próprio, compilação JIT experimental e ferramentas para desenvolvimento.

O projeto busca oferecer um ambiente de execução leve, extensível e multiplataforma, com foco em desempenho, integração nativa e compatibilidade com ambientes Linux e Android.

«🚧 NG está em desenvolvimento ativo. A sintaxe, APIs, módulos, recursos e suporte a plataformas podem sofrer alterações.»

---

🚀 Sobre o projeto

O NG Language Engine é um projeto independente de linguagem de programação que combina um runtime desenvolvido em C com diferentes tecnologias de execução.

O objetivo é construir uma linguagem que possa executar programas em ambientes leves, integrar bibliotecas nativas e evoluir para uma plataforma de desenvolvimento versátil.

Principais recursos

- Engine desenvolvido em C
- Interpretador baseado em AST
- Runtime nativo
- Compilador JIT experimental
- Transpilação para Lua
- Arquitetura modular
- Integração com bibliotecas nativas
- Suporte a JSON
- Integração com SQLite
- Recursos HTTP e networking
- Testes e benchmarks
- Suporte ARM64
- Compatibilidade com ambientes Linux e Android

A disponibilidade de cada recurso depende da versão, compilação e plataforma.

---

📱 Plataformas

Plataforma| Status
Android| Ambiente de desenvolvimento principal
Termux| Suporte principal
Linux ARM64| Suporte e testes
Linux x86_64| Compatibilidade de compilação em testes
Android com root| Ambiente compatível; root não é obrigatório
proot| Compatibilidade em testes
chroot| Compatibilidade em testes
Windows| Portabilidade futura / ainda não validada
macOS| Portabilidade futura / ainda não validada

O NG prioriza inicialmente Android/Termux e Linux.

---

📦 Instalação

Android — Termux

1. Instalar o Termux

Recomendamos obter o Termux por uma fonte oficial:

- F-Droid
- GitHub oficial do Termux

Evite versões antigas distribuídas em lojas desatualizadas.

2. Atualizar o ambiente

pkg update && pkg upgrade

3. Instalar dependências

pkg install git clang make pkg-config openssl libcurl sqlite lua

4. Clonar o repositório

git clone https://github.com/Migu3873378373737/NG.git

5. Entrar no projeto

cd NG

6. Compilar

make

Para uma compilação limpa:

make clean
make

7. Executar

Utilize o launcher do projeto:

bash ng.sh executar exemplos/hello.ng

O launcher foi projetado para auxiliar na execução em ambientes Android onde o armazenamento compartilhado pode possuir restrições de execução direta de binários.

---

🐧 Linux — Debian / Ubuntu

1. Instalar dependências

sudo apt update
sudo apt install git build-essential clang make pkg-config \
libssl-dev libcurl4-openssl-dev libsqlite3-dev liblua5.4-dev

2. Clonar o repositório

git clone https://github.com/Migu3873378373737/NG.git

3. Entrar no diretório

cd NG

4. Compilar

make

5. Executar

Se o binário estiver disponível e o sistema permitir a execução:

./bin/ng executar exemplos/hello.ng

Se necessário, utilize o launcher:

bash ng.sh executar exemplos/hello.ng

---

🐧 Arch Linux

Instalar dependências

sudo pacman -S git base-devel clang openssl curl sqlite lua

Clonar

git clone https://github.com/Migu3873378373737/NG.git
cd NG

Compilar

make

Executar

./bin/ng executar exemplos/hello.ng

---

📱 Linux em Android — proot

É possível compilar o NG dentro de distribuições Linux executadas por proot, como Ubuntu ou Debian no Android.

Instale os pacotes de desenvolvimento dentro da distribuição e siga o procedimento de instalação Linux.

git clone https://github.com/Migu3873378373737/NG.git
cd NG
make

Execute o programa:

./bin/ng executar exemplos/hello.ng

A disponibilidade de recursos nativos, permissões de memória executável e JIT pode variar de acordo com a distribuição e as limitações do Android.

---

🔒 Linux em Android — chroot

Em ambientes chroot, a instalação segue o procedimento Linux convencional.

git clone https://github.com/Migu3873378373737/NG.git
cd NG
make

A compilação depende das ferramentas e bibliotecas instaladas dentro do ambiente.

Root não garante automaticamente que todas as operações de memória executável estejam permitidas.

---

🪟 Windows

O suporte nativo ao Windows ainda não está oficialmente validado.

Possíveis ambientes de portabilidade incluem:

- MSYS2
- MinGW-w64
- LLVM/Clang

A compatibilidade depende da implementação das dependências, do sistema de compilação e das APIs utilizadas pelo engine.

Instruções oficiais para Windows serão disponibilizadas após a validação de uma compilação nativa.

---

⚡ Primeiros passos

Executar um exemplo

Após clonar e compilar o NG, execute um dos exemplos disponíveis no repositório:

bash ng.sh executar exemplos/hello.ng

No Linux, quando o binário puder ser executado diretamente:

./bin/ng executar exemplos/hello.ng

Criar seu próprio programa

Crie um arquivo com extensão ".ng":

nano meu_programa.ng

Escreva seu código usando a sintaxe da versão atual do NG.

Execute:

bash ng.sh executar meu_programa.ng

Consulte a pasta "exemplos/" para programas funcionais e atualizados.

---

⚙️ Engine de execução

O NG possui diferentes componentes de execução.

Interpretador

O interpretador C executa programas por meio da estrutura AST e do runtime da linguagem.

É a alternativa de execução mais compatível com ambientes que apresentam restrições para código nativo gerado dinamicamente.

JIT — Just-In-Time Compiler

O NG possui um compilador JIT experimental que busca converter partes compatíveis da execução em código nativo.

O JIT depende das permissões de memória e das capacidades do sistema operacional.

Em Android, Termux, proot e chroot, a execução de código nativo gerado dinamicamente pode estar sujeita a limitações.

O projeto está trabalhando na melhoria da alocação de memória executável, compatibilidade e fallback automático.

Transpiler Lua

O NG também possui recursos de transpilação para Lua, permitindo converter construções suportadas da linguagem para código Lua.

A compatibilidade depende dos recursos implementados no transpiler.

---

🔧 Compilação a partir do código-fonte

Limpar artefatos:

make clean

Compilar:

make

Executar testes:

make test

Executar a suíte completa:

make test-all

Executar benchmark:

make bench

Consultar os targets disponíveis:

make

---

📊 Benchmarks

O NG possui benchmarks que avaliam diferentes operações:

- Loops
- Operações aritméticas
- Condicionais
- Iteração de listas
- Acesso indexado
- Operações com strings
- Loops aninhados

Os resultados dependem de:

- Processador
- Sistema operacional
- Compilador C
- Flags de compilação
- Backend de execução
- Gerenciamento de memória
- Condições do ambiente

Sempre informe o hardware, a versão do NG e o backend utilizado ao divulgar benchmarks.

---

🗺️ Roadmap

- [ ] Estabilização da sintaxe
- [ ] Melhoria de mensagens de erro
- [ ] Expansão da biblioteca padrão
- [ ] Melhoria do compilador JIT
- [ ] Gerenciamento seguro de memória executável
- [ ] Fallback adaptativo entre JIT e interpretador
- [ ] Melhor compatibilidade Android
- [ ] Melhor compatibilidade Linux
- [ ] Portabilidade para Windows
- [ ] Portabilidade para macOS
- [ ] Gerenciador de pacotes
- [ ] Documentação completa
- [ ] Mais benchmarks
- [ ] Builds automatizados
- [ ] CI/CD

---

📚 Documentação

A documentação está sendo organizada para cobrir:

Documento| Conteúdo
"docs/INSTALL.md"| Instalação e compilação
"docs/GETTING_STARTED.md"| Primeiros passos
"docs/LANGUAGE.md"| Fundamentos da linguagem
"docs/SYNTAX.md"| Referência da sintaxe
"docs/BUILTINS.md"| Funções nativas
"docs/MODULES.md"| Módulos e bibliotecas
"docs/JIT.md"| Compilador JIT
"docs/ARCHITECTURE.md"| Arquitetura do engine
"docs/PLATFORM_SUPPORT.md"| Compatibilidade de plataformas
"docs/FAQ.md"| Perguntas frequentes

---

🤝 Contribuindo

Contribuições são bem-vindas!

Você pode contribuir com:

- Relatos de bugs
- Correções
- Documentação
- Exemplos
- Testes
- Melhorias de desempenho
- Bibliotecas
- Compatibilidade com outras plataformas
- Novos recursos da linguagem

Antes de contribuir, consulte as orientações do projeto, quando disponíveis.

---

🐛 Reportando bugs

Ao reportar um problema, inclua:

- Versão ou commit do NG
- Sistema operacional
- Arquitetura
- Versão do compilador
- Comando utilizado
- Mensagem de erro
- Exemplo mínimo que reproduza o problema

Isso facilita a investigação e a correção.

---

🔐 Segurança

Se você encontrar uma vulnerabilidade de segurança, evite publicar detalhes que possam colocar usuários em risco antes que ela seja investigada e corrigida.

Utilize os canais de contato ou o procedimento de segurança do repositório, quando disponível.

---

📄 Licença

Consulte o arquivo "LICENSE" deste repositório para verificar os termos de distribuição e uso do NG.

---

⭐ Projeto

NG Language Engine é um projeto independente em desenvolvimento.

Se você gostou da ideia, acompanhe o desenvolvimento, teste a linguagem, reporte bugs ou contribua com código e documentação.

🔗 Repositório oficial:

https://github.com/Migu3873378373737/NG
