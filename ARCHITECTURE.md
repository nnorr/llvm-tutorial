# Kaleidoscope — Architecture

A modular reimplementation of the [LLVM Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/).

The tutorial ships each chapter as a single `toy.cpp` in which every piece of
state is a file-scope global. That works for a linear read-through but hides how
the components actually relate. This version splits them into modules with
explicit interfaces, which surfaces three couplings the globals concealed —
documented under [Cross-cutting decisions](#cross-cutting-decisions).

It is also a **union of the tutorial's chapters**, which no single chapter is:

|                     | Ch4 | Ch5–7 | Ch8 | Ch9 | here |
| ------------------- | :-: | :---: | :-: | :-: | :--: |
| Function passes     |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| JIT execution       |  ✅  |   ✅   |  ❌  |  ❌  |  ✅   |
| Object emission     |  ❌  |   ❌   |  ✅  |  ❌  |  ✅   |
| Debug info (DWARF)  |  ❌  |   ❌   |  ❌  |  ✅  |  ✅   |

Ch8 drops both the JIT and the optimizer; Ch9 drops them too and adds debug
info. Supporting all four at once is only practical once the backends are
separable, which is the clearest argument for the split.

---

## Module map

```
                    SourceLocation.h
                     ╱            ╲
                Lexer.h          AST.h ←── ASTVisitor.h
                    ╲            ╱   ╲
                     ╲          ╱     ╲
                      Parser.h         CodeGen.h ──→ DebugInfo.h
                          ╲               ╱  ╲
                           ╲             ╱    ╲
                        OperatorTable.h        ObjectEmitter.h
                                    ╲          ╱
                                     ╲        ╱   KaleidoscopeJIT.h
                                      ╲      ╱     ╱
                                       main.cpp ──┘
```

Nothing points upward. `AST.h` is the bottom of the graph and has **zero LLVM
includes**.

| File                     | Lines | Responsibility |
| ------------------------ | ----: | -------------- |
| `include/SourceLocation.h` |   16 | `{Line, Col}` pair; produced by Lexer, carried by AST, consumed by DebugInfo |
| `include/Lexer.h` + `src/Lexer.cpp` | 200 | Character stream → tokens, with location tracking |
| `include/ASTVisitor.h`   |    44 | Double-dispatch interface over the 8 expression nodes |
| `include/AST.h`          |   223 | Node classes. Pure data + `accept()` |
| `include/OperatorTable.h`|    46 | Binary operator precedence, shared by Parser and CodeGen |
| `include/Parser.h` + `src/Parser.cpp` | 473 | Recursive descent + precedence climbing; the only thing that builds AST nodes |
| `include/CodeGen.h` + `src/CodeGen.cpp` | 627 | AST → LLVM IR, as an `ASTVisitor`. Owns the pass pipeline |
| `include/DebugInfo.h` + `src/DebugInfo.cpp` | 127 | DWARF metadata emission; wraps `DIBuilder` |
| `include/ObjectEmitter.h` + `src/ObjectEmitter.cpp` | 101 | Module → native `.o` via `TargetMachine` |
| `src/main.cpp`           |   294 | Argument parsing and the two drivers |
| `include/KaleidoscopeJIT.h` | 105 | **Vendored** from upstream, unmodified |

---

## Data flow

```
                       ┌──────────────────── JIT mode (default) ────────────────┐
                       │                                                        │
stdin/file → Lexer → Parser → AST → CodeGen ──→ Module ──→ KaleidoscopeJIT ──→ evaluate
                                       │                    (ResourceTracker)
                                       │                                        │
                       └──────────────────── compile mode (-c) ─────────────────┘
                                       │
                                  DebugInfo (-g)          ObjectEmitter → output.o
```

The two modes differ in exactly three ways:

1. **Module lifetime.** JIT mode hands off a module and reopens a fresh one
   after *every* definition and *every* top-level expression. Compile mode calls
   `initModule()` once and accumulates everything. This is why `CodeGen` is
   deliberately **not** a construct-once object.

   The two handoffs differ, and the difference matters:

   | | ResourceTracker? | Lifetime |
   | --- | --- | --- |
   | `def` | no | permanent — later expressions call into it |
   | top-level expr | yes | removed right after evaluation |

   A definition must get its own module *immediately*. Left in the working
   module, it would be handed to the JIT as part of the next top-level
   expression's tracked module and then freed with it — and the following call
   would fail with `Symbols not found`. `FunctionProtos` would still re-emit a
   declaration, so this surfaces at JIT lookup, not at codegen.
2. **Top-level wrapper name.** `__anon_expr` for JIT (looked up and called),
   `main` for compile (a real entry point). Hence
   `Parser::parseTopLevelExpr(name)`.
3. **Debug info.** Attached only in compile mode.

### Why `-g` requires `-c`

DWARF describes a source file on disk — file, line, column. A REPL has no such
file, and each JIT'd expression lives in a throwaway module freed right after
evaluation, so metadata describing it would point at code that no longer exists.
The tutorial hits the same wall and hardcodes `createFile("fib.ks", ".")` with a
comment admitting it. Debugging JIT'd code needs the GDB JIT registration
interface, which is out of scope here.

Compile mode also accepts **only one top-level expression**, because it becomes
`main` and a second would redefine it.

---

## The visitor pattern

The tutorial hangs `virtual Value *codegen()` off each AST node. That is exactly
why its `IRBuilder`, `NamedValues`, and `TheModule` had to be globals: the nodes
needed backend state and had no way to receive it.

Here the nodes are passive:

```cpp
// AST.h — no LLVM headers
class NumberExprAST : public ExprAST {
  double Val;
public:
  void accept(ASTVisitor &V) override { V.visit(*this); }
  double getVal() const { return Val; }
};
```

and each consumer carries its own state:

```cpp
// CodeGen.h
class CodeGen : public ASTVisitor {
  std::unique_ptr<llvm::IRBuilder<>> Builder;
  std::map<std::string, llvm::AllocaInst *> NamedValues;
  llvm::Value *Result = nullptr;
  void visit(NumberExprAST &E) override;
  ...
};
```

### The `Result` member

`visit()` cannot return `llvm::Value*` — that is precisely the type the AST must
not know about. So visitors that produce a value stash it and expose a typed
wrapper:

```cpp
llvm::Value *CodeGen::codegenExpr(ExprAST &E) {
  Result = nullptr;
  E.accept(*this);
  return Result;
}
```

This is safe under recursion only because every caller reads `Result` into a
local *immediately*. `visit(BinaryExprAST&)` does:

```cpp
Value *L = codegenExpr(E.getLHS());   // captured before...
Value *R = codegenExpr(E.getRHS());   // ...this overwrites Result
```

If you add a node, keep that discipline — leaving a value in `Result` across a
nested `codegenExpr()` call is the one way to break this design.

`PrototypeAST` and `FunctionAST` do **not** derive from `ExprAST` and are not in
the visitor. `CodeGen` handles them through overloads:
`codegen(PrototypeAST&)` and `codegen(FunctionAST&)`.

---

## Cross-cutting decisions

### `OperatorTable` — breaking a dependency cycle

`FunctionAST::codegen` *writes* the precedence table (Ch7 lines 1242, 1310):
a user-defined operator (`def binary | 5 (LHS RHS) ...`) only becomes available
to the parser once its definition has been code-generated. So CodeGen must
mutate parser state — a genuine cycle, invisible while both sides just touched a
global.

`OperatorTable` is owned by `main` and referenced by both. Neither module owns
it, and neither depends on the other.

### `SourceLocation` — the AST must not reach upward

The tutorial writes `ExprAST(SourceLocation Loc = CurLoc)`, defaulting the
parameter to the *lexer's global*. That makes `AST.h` depend on `Lexer`,
inverting the graph. Here the location is a required parameter and the parser
passes `Lex.getCurLoc()` explicitly.

### No anonymous namespace on the AST

The tutorial wraps its AST classes in `namespace { ... }` (internal linkage),
fine inside one translation unit. In a header that would give every `.cpp` its
own distinct `ExprAST`, so `unique_ptr<ExprAST>` in `Parser.h` would not be the
same type as in `main.cpp`. The same trap applies to every `static` global the
tutorial declares — all of them became class members here.

### `CurTok` belongs to the Parser

One-token lookahead is a parsing concern, not a lexing one. The `Lexer` stays a
pure token source; `Parser` owns `CurTok` and `getNextToken()`.

### `Lexer` takes an `std::istream&`

The tutorial reads `getchar()` directly. Taking a stream lets the same lexer
serve stdin (JIT) and a file (compile), and makes it testable from a
`std::istringstream` without driving the whole program.

---

## Deviations from the tutorial

Beyond the structural changes above, six behavioral ones — all deliberate:

1. **Fixed a use-after-move.** Ch9 line 1310 dereferences `Proto` after moving it
   into `FunctionProtos` at line 1235 — a null `unique_ptr` deref on the error
   path. It uses the saved reference `P` everywhere else; now that line does too.
2. **`dynamic_cast` for the `=` LHS check.** The tutorial `static_cast`s and then
   null-checks, which is dead code — `static_cast` never yields null. Conda's
   `llvm-config` does not pass `-fno-rtti`, so the real check works. If you ever
   build against an LLVM configured without RTTI, this is the one line to revisit.
3. **Native target init, not `InitializeAll*`.** We only emit for the host, so
   this links the `native` component instead of every backend.
4. **Source locations captured at the keyword.** The tutorial let `for`/`var`/unary
   nodes default their location to wherever the lexer had reached by construction
   time — the *end* of the construct. Taking it at the keyword gives useful line
   info.
5. **Optimization disabled under `-g`.** Matches Ch9, which runs no passes, and
   keeps line tables readable in a debugger.
6. **`ENABLE_EXPORTS`** on the `toy` target. The JIT resolves `extern` declarations
   (`putchard`, `printd`) against the running process, so the executable's own
   symbols must be in the dynamic symbol table. Without it, `extern putchard(x);`
   links but fails to resolve at runtime.

---

## Mapping to the tutorial

| Chapter | Lands in |
| ------- | -------- |
| 1 — Lexer | `Lexer.{h,cpp}` |
| 2 — Parser / AST | `AST.h`, `ASTVisitor.h`, `Parser.{h,cpp}`, `OperatorTable.h` |
| 3 — IR generation | `CodeGen::visit(...)`, `codegen(PrototypeAST&)`, `codegen(FunctionAST&)` |
| 4 — JIT + optimizer | `CodeGen::initModule` (pass pipeline), `runInteractive()` in `main.cpp` |
| 5 — Control flow | `visit(IfExprAST&)`, `visit(ForExprAST&)` |
| 6 — User-defined operators | `visit(UnaryExprAST&)`, `OperatorTable`, `Parser::parsePrototype` |
| 7 — Mutable variables | `visit(VarExprAST&)`, `CodeGen::createEntryBlockAlloca`, `PromotePass` |
| 8 — Object code | `ObjectEmitter.{h,cpp}`, `runCompile()` |
| 9 — Debug info | `DebugInfo.{h,cpp}`, `SourceLocation.h`, `-g` path |

---

## Two things worth knowing

**`createEntryBlockAlloca` always inserts into the entry block**, never the
current one. That is a `mem2reg` (`PromotePass`) requirement, not a style
choice — allocas elsewhere are not promoted, and the mutable-variable design of
Ch7 silently degrades.

**Kaleidoscope's `for` is a do-while**, and the end condition is evaluated
*before* the induction variable is incremented:

```
CreateBr(LoopBB)              // unconditional — no guard on entry
  body; step
  EndCond = ...               // condition computed here...
  NextVar = CurVar + Step     // ...then the variable is incremented
CreateCondBr(EndCond, LoopBB, AfterBB)
```

So `for i = 1, i < 1 in ...` still runs the body **once**, and the loop runs one
more iteration than the equivalent C `for` would.

---

## Build & run

```bash
conda activate llvm-tut          # LLVM 20.1.8, clang++ 20.1.8, gcc 16.1.0
cmake -S . -B build -G Ninja
cmake --build build
```

Activation exports `CXX`, so CMake picks up conda's **gcc 16.1.0** by default.
Both compilers are verified to build and run this; for clang++ instead, add
`-DCMAKE_CXX_COMPILER=clang++` (only honored on the first configure — CMake
caches it, so delete `build/` to switch).

`CMakeLists.txt` pins `CMAKE_BUILD_TYPE=Debug` and forces `-O0` into
`CMAKE_CXX_FLAGS_DEBUG`. That second part is load-bearing: conda's activation
exports `CXXFLAGS` containing `-O2`, CMake seeds `CMAKE_CXX_FLAGS` from it, and
those flags are emitted *before* the per-config ones — so `-O0` has to come
later to win.

```bash
./build/toy                              # REPL + JIT
./build/toy -c tests/fib.ks              # -> output.o
./build/toy -c tests/fib.ks -g -o fib.o  # with DWARF
```

`build.sh` remains for compiling the single-file reference chapters:

```bash
./build.sh ch7.cpp                       # -> ./ch7, using clang++
TOY_CXX="$CXX" ./build.sh ch7.cpp        # ...or conda gcc
```

Upstream sources for every chapter are vendored under
`reference/kaleidoscope/`, taken from `release/20.x` to match the local
toolchain. They are the oracle: when this implementation misbehaves, diff
against the chapter that covers the feature.
