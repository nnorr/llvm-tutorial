## Setup

```bash
conda create -y -n llvm-tut -c conda-forge 'llvmdev=20' 'clangxx=20' \
    gxx_linux-64 cmake ninja llvm-tools lit
conda activate llvm-tut
```

Any LLVM 20 install works; if CMake cannot find it, add
`-DCMAKE_PREFIX_PATH=<prefix>` to the configure step below.

## Build and test

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure     # 4/4 pass
```

Three suites: `lexer` (unit tests), `jit_fib` / `jit_operators` (end-to-end),
and `lit` (IR tests). `llvm-tools` and `lit` are only needed for the last one --
without them CMake reports `IR tests disabled` and the rest still run.

```bash
lit -v build/test                      # the IR suite alone
lit -v build/test/codegen/mem2reg.ks   # one file
```

IR tests go through the build tree, not `test/`, because that is where CMake
writes the generated `lit.site.cfg.py` holding the path to `toy`.

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


