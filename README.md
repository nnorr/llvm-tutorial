# Kaleidoscope

Working through the [LLVM Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/),
as a stepping stone to MLIR.

The tutorial ships each chapter as one `toy.cpp` where every piece of state is a
file-scope global. This repo reimplements it as separate modules — Lexer,
Parser, AST, CodeGen, DebugInfo, ObjectEmitter — with the AST as passive data
and each consumer written as a visitor.

It also unifies features the tutorial splits across chapters. No single upstream
chapter has both the optimizer and object emission:

|                    | Ch4 | Ch5–7 | Ch8 | Ch9 | here |
| ------------------ | :-: | :---: | :-: | :-: | :--: |
| Function passes    |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| JIT execution      |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| Object emission    |  ❌  |   ❌   |  ✅  |  ❌  |  ✅   |
| Debug info (DWARF) |  ❌  |   ❌   |  ❌  |  ✅  |  ✅   |

## Quick start

```bash
conda activate llvm-tut
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires LLVM 20 development headers. To create the environment from scratch:

```bash
conda create -y -n llvm-tut -c conda-forge 'llvmdev=20' 'clangxx=20' \
    gxx_linux-64 cmake ninja
```

## Usage

```bash
./build/toy                                  # REPL, JIT-evaluates as you type
./build/toy < tests/fib.ks                   # -> Evaluated to 89.000000
./build/toy -c tests/fib.ks -o fib.o         # compile to a native object
./build/toy -c tests/fib.ks -g -o fib.o      # ...with DWARF debug info
./build/toy --dump-ast < tests/fib.ks        # print the parse tree
```

`-g` requires `-c`: DWARF describes a source file on disk, which a REPL does not
have.

## Layout

```
include/  src/        the implementation, one module per component
tests/                lexer unit tests + .ks programs wired into ctest
reference/kaleidoscope/
                      upstream Chapter2-9 toy.cpp, release/20.x, unmodified
ch2.cpp .. ch9.cpp    personal working copies of the tutorial chapters
build.sh              compiles a single-file chapter (not the modular build)
```

`reference/` is the oracle — pinned to `release/20.x` to match the toolchain, so
when something here misbehaves you can diff against the chapter that covers it.

```bash
./build.sh ch7.cpp && ./ch7 < tests/fib.ks   # run the upstream version
```

## Where to look

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
