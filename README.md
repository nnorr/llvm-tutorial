# Kaleidoscope

A modular reimplementation of the
[LLVM Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/).
Unlike any single upstream chapter, it has the optimizer, the JIT, object
emission and DWARF debug info at the same time.

Design notes: [ARCHITECTURE.md](ARCHITECTURE.md).
Code walkthrough (Korean): [docs/](docs/README.md).

## Setup

```bash
conda create -y -n llvm-tut -c conda-forge 'llvmdev=20' 'clangxx=20' \
    gxx_linux-64 cmake ninja
conda activate llvm-tut
```

Any LLVM 20 install works; if CMake cannot find it, add
`-DCMAKE_PREFIX_PATH=<prefix>` to the configure step below.

## Build and test

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure     # 3/3 pass
```

## Run

```bash
# REPL: JIT-compiles and evaluates as you type, prints all IR at EOF
./build/toy

# JIT a file
./build/toy < tests/fib.ks                     # Evaluated to 89.000000
./build/toy < tests/operators.ks               # 9 values, last is 1.000000

# Native object file
./build/toy -c tests/fib.ks -o /tmp/fib.o      # Wrote /tmp/fib.o
nm -g /tmp/fib.o                               # binary:, fib, main

# ...with DWARF
./build/toy -c tests/fib.ks -g -o /tmp/fibg.o
readelf --debug-dump=info        /tmp/fibg.o | head -14
readelf --debug-dump=decodedline /tmp/fibg.o | head -8

# Parse tree
./build/toy --dump-ast < tests/fib.ks

# LLVM IR alongside the object file
./build/toy -c tests/fib.ks --emit-llvm -o /tmp/fib.o
```

The top-level expression in a `.ks` file becomes `main`, so the object links on
its own. To call a function from C, drop that line first:

```bash
head -5 tests/fib.ks > /tmp/fibonly.ks
./build/toy -c /tmp/fibonly.ks -o /tmp/fibonly.o
printf '#include <stdio.h>\ndouble fib(double);\nint main(void){printf("%%f\\n",fib(10));}\n' > /tmp/driver.c
clang /tmp/driver.c /tmp/fibonly.o -o /tmp/fibprog && /tmp/fibprog   # 89.000000
```

`-g` disables the IR passes, because mem2reg would delete the allocas that
`dbg.declare` points at ([details](docs/10-debuginfo-and-optimization.md)).
Plain `-c` still optimizes:

```bash
./build/toy -c tests/fib.ks    -o /tmp/a.o --emit-llvm 2>/tmp/plain.ll
./build/toy -c tests/fib.ks -g -o /tmp/b.o --emit-llvm 2>/tmp/dbg.ll
grep -c alloca /tmp/plain.ll /tmp/dbg.ll       # 0 and 7
```

## Options

```
usage: toy [-c <file.ks> [-g] [-o <file.o>]]
  no arguments   read stdin, JIT and evaluate interactively
  -c <file.ks>   compile to a native object file
  -g             emit debug info (disables optimization)
  -o <file.o>    output object name (default output.o)
  --dump-ast     print the AST for each construct
  --emit-llvm    with -c, also print the module's IR
```

Host target only; there is no cross-compilation flag.

## Layout

```
include/  src/    the implementation, one module per component
tests/            lexer unit tests + .ks programs wired into ctest
docs/             per-component code walkthrough (Korean)
reference/        upstream Chapter2-9 toy.cpp, release/20.x, unmodified
ch2.cpp ..        working copies of individual tutorial chapters
build.sh          compiles one chapter file, e.g. ./build.sh ch7.cpp
```

Note: conda's `llvmdev` ships no `llvm-dwarfdump`, `opt` or `llc`, which is why
the commands above use `readelf` and `nm`.
