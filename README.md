NG Language Engine

NG is an experimental programming language and language engine built with C.

NG combines a native execution engine, a C-based interpreter, and experimental compilation technologies with the goal of building a lightweight, extensible and portable programming environment.

«⚠️ NG is currently under active development. Some features, APIs, syntax and platform support may change between versions.»

---

🚀 Features

- C-based language engine
- AST-based execution
- Native interpreter
- Experimental JIT compiler
- Lua transpilation support
- Modular architecture
- Native library integration
- JSON support
- SQLite integration
- HTTP/networking support
- Benchmarking tools
- ARM64 support
- Designed to run in lightweight environments

Feature availability may depend on the current build, platform and project version.

---

📱 Supported Platforms

Platform| Status
Android / Termux| Primary development environment
Linux ARM64| Supported / testing
Linux x86_64| Build compatibility testing
Android with root| Supported environment, root not required for basic use
proot| Compatibility testing
chroot| Compatibility testing
Windows| Planned / porting status to be confirmed
macOS| Planned / porting status to be confirmed

Platform support is continuously evolving.

---

📦 Installation

Android — Termux

Install Termux from a trusted source, preferably F-Droid or the official Termux GitHub repository.

Update packages:

pkg update && pkg upgrade

Install dependencies:

pkg install git clang make pkg-config
pkg install openssl libcurl sqlite lua

Clone NG:

git clone https://github.com/USERNAME/ng-language.git
cd ng-language

Compile:

make

Run:

bash ng.sh executar exemplos/hello.ng

«Replace "USERNAME/ng-language" with the actual repository address.»

Linux — Debian / Ubuntu

Install dependencies:

sudo apt update
sudo apt install git build-essential clang make pkg-config
sudo apt install libssl-dev libcurl4-openssl-dev libsqlite3-dev

Install Lua development files according to your distribution.

Clone:

git clone https://github.com/USERNAME/ng-language.git
cd ng-language

Compile:

make

Run a program:

./bin/ng executar exemplos/hello.ng

If the executable cannot run directly from the current filesystem, use the project's launcher where applicable.

Arch Linux

Install the build dependencies:

sudo pacman -S git base-devel clang openssl curl sqlite lua

Clone and build:

git clone https://github.com/USERNAME/ng-language.git
cd ng-language
make

Windows

Windows support depends on the current compatibility of the source code and dependencies.

Recommended development environments:

- MSYS2 / MinGW
- LLVM / Clang
- CMake or Make, depending on the build system
- Required native libraries

Windows native installation instructions will be published once the build is verified on Windows.

proot / chroot

NG can be built inside Linux environments running under Android, including proot or chroot, provided the required compiler and libraries are available.

git clone https://github.com/USERNAME/ng-language.git
cd ng-language
make

If native JIT memory execution is unavailable, the engine should use the compatible execution backend when supported by the current version.

---

⚡ Quick Start

Create a program:

nano hello.ng

Example program:

// Hello World
// Replace this example with the current valid NG syntax.

Execute:

bash ng.sh executar hello.ng

---

📚 Documentation

Document| Description
"Installation" (docs/INSTALL.md)| Installation and compilation
"Getting Started" (docs/GETTING_STARTED.md)| First steps with NG
"Language Guide" (docs/LANGUAGE.md)| Language fundamentals
"Syntax" (docs/SYNTAX.md)| Syntax reference
"Built-in Functions" (docs/BUILTINS.md)| Built-in functions
"Modules" (docs/MODULES.md)| Modules and libraries
"JIT" (docs/JIT.md)| JIT architecture and execution
"Architecture" (docs/ARCHITECTURE.md)| Internal engine architecture
"Platform Support" (docs/PLATFORM_SUPPORT.md)| Platform-specific information
"FAQ" (docs/FAQ.md)| Frequently asked questions

---

🧠 Execution Engine

NG is designed around multiple execution technologies.

Interpreter

The interpreter executes programs through the language engine and AST-based runtime.

JIT Compiler

NG includes an experimental Just-In-Time compiler that generates native machine code.

JIT execution depends on platform-specific memory permissions and runtime restrictions. Environments that cannot safely execute generated code may use a compatible fallback backend.

Lua Transpiler

NG includes Lua transpilation capabilities, allowing supported language constructs to be translated into Lua code.

---

🔧 Building from Source

Clean previous build artifacts:

make clean

Compile:

make

Run tests:

make test

Run the complete test suite:

make test-all

Run benchmarks:

make bench

Check available Make targets:

make

---

📊 Benchmarks

NG includes performance tests for operations such as:

- Loops
- Arithmetic
- Conditional execution
- List iteration
- Indexed access
- String operations
- Nested loops

Benchmarks depend on hardware, operating system, compiler optimization, runtime backend and memory conditions.

Benchmark results should always identify the execution backend and test environment.

---

🗺️ Roadmap

- [ ] Stabilize language syntax
- [ ] Improve error messages
- [ ] Expand standard library
- [ ] Improve JIT memory management
- [ ] Adaptive JIT fallback
- [ ] Improve Linux compatibility
- [ ] Improve Android compatibility
- [ ] Windows port
- [ ] macOS port
- [ ] Package manager
- [ ] Comprehensive language documentation
- [ ] More benchmarks
- [ ] Automated CI builds

---

🤝 Contributing

Contributions are welcome.

You can contribute by:

- Reporting bugs
- Improving documentation
- Adding examples
- Writing tests
- Improving performance
- Developing libraries
- Implementing platform support

Please read "CONTRIBUTING.md" (CONTRIBUTING.md) before submitting changes.

---

🐛 Reporting Bugs

When reporting a bug, include:

- NG version or commit
- Operating system
- Architecture
- Compiler version
- Command used
- Error output
- Minimal reproducible example

---

🔒 Security

Please report security vulnerabilities responsibly.

See "SECURITY.md" (SECURITY.md).

---

📄 License

NG is distributed under the license specified in the repository's "LICENSE" file.

---

⭐ About

NG Language Engine is an independent programming language project under active development.

If you find the project interesting, consider starring the repository and contributing to its development.
