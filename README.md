# Kaleidoscope

A reimplementation of the [LLVM Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/),
as a stepping stone to MLIR.

The tutorial ships each chapter as one `toy.cpp` where every piece of state is a
file-scope global. This repo splits it into modules — Lexer, Parser, AST,
CodeGen, DebugInfo, ObjectEmitter — with the AST as passive data and each
consumer written as a visitor.

It also unifies features the tutorial splits across chapters. No single upstream
chapter has both the optimizer and object emission:

|                    | Ch4 | Ch5–7 | Ch8 | Ch9 | here |
| ------------------ | :-: | :---: | :-: | :-: | :--: |
| Function passes    |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| JIT execution      |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| Object emission    |  ❌  |   ❌   |  ✅  |  ❌  |  ✅   |
| Debug info (DWARF) |  ❌  |   ❌   |  ❌  |  ✅  |  ✅   |

## Requirements

LLVM 20 development headers, CMake ≥ 3.20, Ninja, and a C++17 compiler.
Any LLVM 20 install works. With conda:

```bash
conda create -y -n llvm-tut -c conda-forge 'llvmdev=20' 'clangxx=20' \
    gxx_linux-64 cmake ninja
conda activate llvm-tut
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Configuration prints the LLVM it found:

```
-- Using LLVM 20.1.8 from /path/to/env/lib/cmake/llvm
```

If CMake cannot find LLVM, point it at the install explicitly:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=$CONDA_PREFIX
```

## Verify

```bash
ctest --test-dir build --output-on-failure
```

```
    Start 1: lexer
1/3 Test #1: lexer ............................   Passed
    Start 2: jit_fib
2/3 Test #2: jit_fib ..........................   Passed
    Start 3: jit_operators
3/3 Test #3: jit_operators ....................   Passed

100% tests passed, 0 tests failed out of 3
```

`lexer` is a unit test; the other two JIT a `.ks` program and check the value it
evaluates to.

## Reproducing each mode

Every command below is run from the repo root and prints what is shown.

### 1. REPL with JIT

```bash
./build/toy
```

Type an expression and it is compiled and executed immediately:

```
ready> def double(x) x * 2;
Read function definition:define double @double(double %x) {
entry:
  %multmp = fmul double %x, 2.000000e+00
  ret double %multmp
}

ready> double(21);
Evaluated to 42.000000
ready> ^D
```

Piping a file works the same way. `tests/fib.ks` computes the 10th Fibonacci
number with a mutable-variable loop and a user-defined `:` operator:

```bash
./build/toy < tests/fib.ks 2>&1 | grep Evaluated
```

```
ready> Evaluated to 89.000000
```

`tests/operators.ks` exercises user-defined unary and binary operators,
`if`/`then`/`else`, and `for` with a step:

```bash
./build/toy < tests/operators.ks 2>&1 | grep Evaluated
```

```
ready> Evaluated to 1.000000
ready> Evaluated to -5.000000
ready> Evaluated to 1.000000
ready> Evaluated to 1.000000
ready> Evaluated to -1.000000
ready> Evaluated to 0.000000
ready> Evaluated to 1.000000
ready> Evaluated to 30.000000
ready> Evaluated to 1.000000
```

One line per expression at the bottom of the file: `!0`, `-(5)`, `7 > 3`,
`0 | 3`, `nested(-4)`, `nested(5)`, `nested(50)`, `sumstep(10)`,
`(1 < 2) | (5 > 9)`.

At EOF the REPL prints the IR of everything it generated.

### 2. Object file

```bash
./build/toy -c tests/fib.ks -o /tmp/fib.o
```

```
Wrote /tmp/fib.o
```

A top-level expression becomes `main`, so the object is directly linkable:

```bash
nm -g /tmp/fib.o
```

```
0000000000000000 T binary:
0000000000000010 T fib
0000000000000090 T main
```

To call `fib` from C instead, drop the last line of the program so no `main` is
generated:

```bash
head -5 tests/fib.ks > /tmp/fibonly.ks
./build/toy -c /tmp/fibonly.ks -o /tmp/fibonly.o

cat > /tmp/driver.c <<'EOF'
#include <stdio.h>
double fib(double);
int main(void) { printf("%f\n", fib(10)); return 0; }
EOF

clang /tmp/driver.c /tmp/fibonly.o -o /tmp/fibprog && /tmp/fibprog
```

```
89.000000
```

### 3. Debug info

```bash
./build/toy -c tests/fib.ks -g -o /tmp/fibg.o
readelf --debug-dump=info /tmp/fibg.o | head -14
```

```
Contents of the .debug_info section:

  Compilation Unit @ offset 0x0:
   Length:        0xa5 (32-bit)
   Version:       4
   Abbrev Offset: 0x0
   Pointer Size:  8
 <0><b>: Abbrev Number: 1 (DW_TAG_compile_unit)
    <c>   DW_AT_producer    : (indirect string, offset: 0x0): Kaleidoscope Compiler
    <10>   DW_AT_language    : 2	(non-ANSI C)
    <12>   DW_AT_name        : (indirect string, offset: 0x16): tests/fib.ks
    <16>   DW_AT_stmt_list   : 0x0
    <1a>   DW_AT_comp_dir    : (indirect string, offset: 0x23): .
    <1e>   DW_AT_GNU_pubnames: 1
```

The line table maps addresses back to source positions:

```bash
readelf --debug-dump=decodedline /tmp/fibg.o | head -8
```

```
Contents of the .debug_line section:

CU: tests/fib.ks:
File name        Line number    Starting address    View
fib.ks                     1                   0
fib.ks                     1                 0xc
fib.ks                     2                0x10
fib.ks                     3                0x1a
```

`-g` requires `-c`: DWARF describes a source file on disk, which a REPL does not
have.

### 4. AST and IR dumps

```bash
./build/toy --dump-ast < tests/fib.ks 2>/dev/null | head -8
```

```
Function
  Prototype binary: (x y) [binary ':' prec 1] @1
  Body:
    Variable y @1:22
Function
  Prototype fib (n) @2
  Body:
    Var @3:3
```

```bash
./build/toy -c tests/fib.ks --emit-llvm -o /tmp/fib.o
```

Prints the whole module to stderr before writing the object.

### 5. Optimization on and off

`-g` turns off the IR pass pipeline, because mem2reg would delete the allocas
that `dbg.declare` points at. Plain `-c` still optimizes:

```bash
./build/toy -c tests/fib.ks    -o /tmp/a.o --emit-llvm 2>/tmp/plain.ll
./build/toy -c tests/fib.ks -g -o /tmp/b.o --emit-llvm 2>/tmp/dbg.ll
grep -c alloca /tmp/plain.ll /tmp/dbg.ll
```

```
/tmp/plain.ll:0
/tmp/dbg.ll:7
```

Full walkthrough of what changes and how real compilers avoid the tradeoff:
[docs/10-debuginfo-and-optimization.md](docs/10-debuginfo-and-optimization.md).

> The conda `llvmdev` package ships libraries and headers but no LLVM command
> line tools, so `llvm-dwarfdump`, `opt` and `llc` are not available in that
> environment. The commands above use `readelf` / `nm` instead.

## All options

```
usage: toy [-c <file.ks> [-g] [-o <file.o>]]
  no arguments   read stdin, JIT and evaluate interactively
  -c <file.ks>   compile to a native object file
  -g             emit debug info (disables optimization)
  -o <file.o>    output object name (default output.o)
  --dump-ast     print the AST for each construct
  --emit-llvm    with -c, also print the module's IR
                 (the REPL always prints it at EOF)
```

Only the host target is supported; there is no cross-compilation flag.

## Layout

```
include/  src/        the implementation, one module per component
tests/                lexer unit tests + .ks programs wired into ctest
docs/                 per-component code walkthrough (Korean)
reference/kaleidoscope/
                      upstream Chapter2-9 toy.cpp, release/20.x, unmodified
ch2.cpp .. ch9.cpp    personal working copies of some tutorial chapters
build.sh              compiles a single-file chapter (not the modular build)
```

`reference/` is the oracle — pinned to `release/20.x` to match the toolchain, so
when something here misbehaves you can diff against the chapter that covers it.

```bash
./build.sh ch7.cpp && ./ch7 < tests/fib.ks   # run the upstream version
```

## Where to look

**[docs/](docs/README.md)** — 컴포넌트별 코드 설명 (한국어). Walks through each
module line by line, assuming no C++ background. Start here to understand *how
the code works*. [docs/SUMMARY.md](docs/SUMMARY.md) is the one-page version.

**[ARCHITECTURE.md](ARCHITECTURE.md)** covers the design: the module dependency
graph, why the visitor pattern removes the globals, the couplings that only
became visible once the file was split, every deliberate deviation from the
tutorial, and a chapter → code mapping.

Two things that bite people, both detailed there:

- `createEntryBlockAlloca` must insert into the **entry** block — a `mem2reg`
  requirement, not a style choice.
- Kaleidoscope's `for` is a **do-while**, and the condition is evaluated *before*
  the induction variable is incremented, so it runs one more iteration than the
  equivalent C loop.
