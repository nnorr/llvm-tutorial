# Kaleidoscope — Architecture

A modular reimplementation of the [LLVM Kaleidoscope tutorial](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/).

The tutorial ships each chapter as a single `toy.cpp` in which every piece of
state is a file-scope global. That works for a linear read-through but hides how
the components actually relate. This version splits them into modules with
explicit interfaces, which surfaces the couplings the globals concealed —
documented under [Cross-cutting decisions](#cross-cutting-decisions).

For a line-by-line walkthrough of each module aimed at readers without a C++
background, see [`docs/`](docs/README.md) (Korean).

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
                    ╲            ╱   ╲ ╲
                     ╲          ╱     ╲ ╰──→ ASTDumper.h
                      Parser.h         CodeGen.h ──→ DebugInfo.h
                          ╲               ╱  ╲
                           ╲             ╱    ╲
                        OperatorTable.h        ObjectEmitter.h
                                    ╲          ╱
                                     ╲        ╱   KaleidoscopeJIT.h
                                      ╲      ╱     ╱
                                       main.cpp ──┘
```

Nothing points upward. `AST.h` is the bottom of the graph and carries **no LLVM
IR dependency** — no `Value*`, no `IRBuilder`, no `codegen()`. Its one LLVM
include is `llvm/Support/Casting.h`, a header-only utility providing
`isa<>`/`dyn_cast<>`/`cast<>` on top of the hand-rolled `Kind` discriminator
(see [LLVM-style RTTI](#llvm-style-rtti)). `ASTDumper` demonstrates the point:
a full consumer of the AST whose own sources include no LLVM header at all —
only `<ostream>`, `AST.h` and `ASTVisitor.h`. `lexer_tests` makes the same point
at link time: `ldd` shows it against `libstdc++` and `libgcc_s`, nothing else.

| File                     | Lines | Responsibility |
| ------------------------ | ----: | -------------- |
| `include/SourceLocation.h` | 16 | `{Line, Col}` pair; produced by Lexer, carried by AST, consumed by DebugInfo |
| `include/Lexer.h` + `src/Lexer.cpp` | 216 | Character stream → tokens, with location tracking |
| `include/ASTVisitor.h`   | 66 | CRTP dispatcher: one `Kind` switch over the 9 expression nodes |
| `include/AST.h`          | 279 | Node classes. Pure data — no `accept()`, no knowledge of consumers |
| `include/OperatorTable.h`| 46 | Binary operator precedence, shared by Parser and CodeGen |
| `include/Parser.h` + `src/Parser.cpp` | 490 | Recursive descent + precedence climbing; the only thing that builds AST nodes |
| `include/ASTDumper.h` + `src/ASTDumper.cpp` | 173 | Second visitor: prints the AST as a tree. No LLVM dependency |
| `include/CodeGen.h` + `src/CodeGen.cpp` | 576 | AST → LLVM IR, as an `ASTVisitor<CodeGen, Value*>`. Owns the pass pipeline |
| `include/DebugInfo.h` + `src/DebugInfo.cpp` | 127 | DWARF metadata emission; wraps `DIBuilder` |
| `include/ObjectEmitter.h` + `src/ObjectEmitter.cpp` | 101 | Module → native `.o` via `TargetMachine` |
| `src/main.cpp`           | 369 | Argument parsing and the two drivers |
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

The two modes differ in exactly four ways:

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
4. **IR output.** The REPL prints every module's IR at EOF, unconditionally —
   Ch3/Ch9 do this and Ch4–7 do not, because only the former never hand a module
   away. Since ours does, it captures each module's text at handoff and prints
   the lot at end of input. Compile mode follows Ch8 instead (object file, no
   IR) and puts the dump behind `--emit-llvm`; Ch8 and Ch9 cannot both be
   satisfied, as Ch9 prints IR but writes no `.o`.

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

Here the nodes are passive — and carry nothing for a consumer's benefit, not
even an `accept()`:

```cpp
// AST.h — no LLVM IR headers, and no include of ASTVisitor.h
class NumberExprAST : public ExprAST {
  double Val;
public:
  double getVal() const { return Val; }
  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Number; }
};
```

Dispatch lives entirely in `ASTVisitor.h`, as one switch over the `Kind`
discriminator, reached through CRTP:

```cpp
template <typename Derived, typename RetTy = void> class ASTVisitor {
  Derived &derived() { return *static_cast<Derived *>(this); }
public:
  RetTy visit(ExprAST &E) {
    switch (E.getKind()) {
    case ExprAST::Expr_Number:
      return derived().visitNumber(llvm::cast<NumberExprAST>(E));
    ...
    }
    llvm_unreachable("unknown ExprASTKind");
  }
};
```

and each consumer carries its own state:

```cpp
// CodeGen.h
class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> {
  friend class ASTVisitor<CodeGen, llvm::Value *>;
  std::unique_ptr<llvm::IRBuilder<>> Builder;
  std::map<std::string, llvm::AllocaInst *> NamedValues;
  llvm::Value *visitNumber(NumberExprAST &E);
  ...
};
```

This is the shape Clang uses in `StmtVisitor`. It is deliberately *not* the
textbook `accept()`/`visit()` double dispatch, which this repo used earlier and
then replaced; [docs/09-visitor-evolution.md](docs/09-visitor-evolution.md)
records both designs and why the trade went the way it did.

`PrototypeAST` and `FunctionAST` do **not** derive from `ExprAST` and are not in
the visitor. `CodeGen` handles them through overloads:
`codegen(PrototypeAST&)` and `codegen(FunctionAST&)`.

### Two visitors, two return types

`ASTDumper` is the second consumer, and it exists as much to justify the pattern
as to be useful. Compare:

| | produces | `RetTy` | LLVM headers |
| --- | --- | --- | --- |
| `CodeGen` | `llvm::Value*` | `llvm::Value *` | IR, passes |
| `ASTDumper` | output on an `ostream` | `void` (default) | none |

Two return types over one dispatcher is exactly what a virtual `accept()` could
not express: a virtual function cannot vary its return type per visitor, which
is why the earlier design had to route `Value*` through a member variable.

In the tutorial, `dump()` was a virtual method on every node sitting directly
beside `codegen()`, so the AST carried both concerns at once. Here it carries
neither, and adding a third consumer (a type checker, a constant folder) means
writing one new class and touching no existing node.

The reverse is now the expensive direction: a new node kind means editing the
switch in `ASTVisitor.h` and every visitor. That is a closed-world design, and
a deliberate one — the grammar froze at Ch7. An IR whose operation set is open
(MLIR, where dialects register ops at runtime) cannot be built this way at all;
see the last section of the evolution doc.

## LLVM-style RTTI

Every node carries an `ExprASTKind` tag and a matching `classof()`:

```cpp
class ExprAST {
public:
  enum ExprASTKind { Expr_Number, Expr_Variable, ..., Expr_Assign, ... };
  ExprASTKind getKind() const { return Kind; }
};

class VariableExprAST : public ExprAST {
public:
  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Variable; }
};
```

That is all `isa<>`, `dyn_cast<>` and `cast<>` need. LLVM uses this rather than
C++ RTTI because it is normally built `-fno-rtti`, so `dynamic_cast` is
unavailable — and because a tag comparison is far cheaper than a `dynamic_cast`
walk. It is also the same shape MLIR's Toy AST uses, so it transfers directly.

`ASTVisitor::visit` is built on the same `Kind` tag, so consumers rarely reach
for `dyn_cast` directly. It is used in exactly one place,
`Parser::parseBinOpRHS`, to check that the destination of `=` is an identifier.

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

Beyond the structural changes above, nine behavioral ones — all deliberate:

1. **Fixed a use-after-move.** Ch9 line 1310 dereferences `Proto` after moving it
   into `FunctionProtos` at line 1235 — a null `unique_ptr` deref on the error
   path. It uses the saved reference `P` everywhere else; now that line does too.
2. **The `=` destination check moved to the parser.** The tutorial routes
   assignment through `BinaryExprAST` and, in codegen, `static_cast`s the LHS to
   `VariableExprAST` and null-checks it — dead code, since `static_cast` never
   yields null.

   But "the destination of `=` must be an identifier" is a *syntactic* rule, so
   the parser enforces it and emits a dedicated `AssignExprAST` holding the name
   as a `std::string`. Codegen then has no cast and no failure path at all: an
   assignment to a non-variable is **unrepresentable**, not merely rejected
   later. The check itself uses `dyn_cast` (LLVM RTTI, not C++), so nothing here
   depends on how LLVM was configured.

   ```
   2 = 3;   →  Error: destination of '=' must be a variable   (at parse time)
   ```
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
7. **Malformed number literals are rejected.** The tutorial lexes `[0-9.]+` and
   hands it to `strtod` ignoring where parsing stopped, so `1.23.45.67` silently
   becomes `1.23` — everything from the second `.` is swallowed and lost without
   a word. Checking `strtod`'s end pointer makes it loud:

   ```
   1.23.45.67;   →  Error: invalid number literal '1.23.45.67' at 1:1
   ```

   The whole run stays one token rather than being re-split at the second `.`, so
   a typo yields one diagnostic here instead of a cascade of parse errors.

   A rejected token still returns `tok_number` with `NumVal = 0.0` so lexing can
   continue, which means the parser cannot tell it from a valid literal. The
   diagnostic alone therefore let `-c` write an object for a program it had
   already rejected. `Lexer::hadError()` closes that: `runCompile` consults it
   before emitting, while the REPL ignores it and keeps taking input. The gap
   surfaced when `test/driver/bad-number.ks` was written.
8. **The REPL prints all generated IR at EOF.** Ch3/Ch9 end with
   `TheModule->print(errs(), nullptr)`; Ch4–7 cannot, because the JIT has taken
   every module by then. Ours captures the text at each handoff and prints it
   together at end of input, so a session ends with the same overview.
9. **One prompt per construct.** The tutorial prints `ready> ` at the top of
   every `MainLoop` iteration, and the parser does not consume the trailing
   `;` — so a normal session shows `ready> ready> `. Here `case ';'` skips the
   prompt. Everything else in the REPL output is byte-identical to Ch7; the
   comparison recipe is in
   [docs/06-backend-and-driver.md](docs/06-backend-and-driver.md).

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

| Test | What it covers |
| --- | --- |
| `lexer` | 5 unit tests, 87 checks — keywords vs identifiers, number literals incl. the malformed case and `hadError()`, operators as raw ASCII, comments/EOF, line-column tracking |
| `jit_fib` | `tests/fib.ks` end to end → 89 |
| `jit_operators` | `tests/operators.ks` — user-defined unary/binary operators with custom precedence, nested if/else, `for` with explicit step → 30 |
| `lit` | 10 IR tests under `test/`, see below |

### IR tests (`lit` + `FileCheck`)

The three tests above check that the compiler *runs*. They say nothing about
the IR it produces, which for a frontend is the actual contract. `test/` fills
that in using the `llvm/test` idiom: each `.ks` file carries its own `RUN:`
line and its own `CHECK:` expectations, `lit` executes them and `FileCheck`
matches the output.

```
test/codegen/     mem2reg, the folded bool round trip, user-operator lowering
test/debuginfo/   DWARF metadata under -g, no metadata without it,
                  and that -g really does leave the allocas in place
test/driver/      top-level expr becomes main, the one-main restriction,
                  -g requiring -c, malformed literals failing the build
```

Adding a test is dropping in a file — no rebuild, no C++.

```bash
lit -v build/test                      # the suite alone
lit -v build/test/codegen/mem2reg.ks   # one file
```

Tests run out of the *build* tree because that is where CMake writes the
generated `lit.site.cfg.py` carrying the path to `toy`. Both tools are
optional: without them CMake reports `IR tests disabled` and the other three
suites still run.

These earn their keep. Two deliberate mutations — forcing optimization off
always, and forcing it on under `-g` — were caught by exactly the tests that
should catch them (`mem2reg`, `binop-fold`, `user-operators` for the first;
`no-opt`, `dwarf-metadata` for the second). And writing `bad-number.ks` is what
exposed the `hadError()` gap in deviation 7 above.

`lexer_tests` links only `src/Lexer.cpp` and **no LLVM libraries at all** — the
Lexer depends on nothing but `SourceLocation.h`. It uses a few macros rather
than a test framework, keeping the build dependency-free.

Being able to unit-test the lexer at all is the payoff from having it take an
`std::istream&`: tests drive it from a `std::istringstream` with no process
involved. The tutorial's `getchar()`-based lexer can only be exercised by piping
text through the whole program and reading the output by eye.

`sumstep(10)` in `operators.ks` is expected to be **30, not 20** — the do-while
`for` runs one extra iteration. That makes it an accidental regression test for
the loop semantics described above.

---

## Mapping to the tutorial

| Chapter | Lands in |
| ------- | -------- |
| 1 — Lexer | `Lexer.{h,cpp}` |
| 2 — Parser / AST | `AST.h`, `ASTVisitor.h`, `Parser.{h,cpp}`, `OperatorTable.h` |
| 3 — IR generation | `CodeGen::visitNumber(...)` etc., `codegen(PrototypeAST&)`, `codegen(FunctionAST&)` |
| 4 — JIT + optimizer | `CodeGen::initModule` (pass pipeline), `runInteractive()` in `main.cpp` |
| 5 — Control flow | `visitIf`, `visitFor` |
| 6 — User-defined operators | `visitUnary`, `OperatorTable`, `Parser::parsePrototype` |
| 7 — Mutable variables | `visitVar`, `AssignExprAST`, `CodeGen::createEntryBlockAlloca`, `PromotePass` |
| 8 — Object code | `ObjectEmitter.{h,cpp}`, `runCompile()` |
| 9 — Debug info | `DebugInfo.{h,cpp}`, `SourceLocation.h`, `-g` path |
| 9 — per-node `dump()` | `ASTDumper.{h,cpp}` — reworked into a visitor, behind `--dump-ast` |

Two pieces correspond to no chapter, added on top:

| Addition | Lands in |
| -------- | -------- |
| LLVM-style RTTI (`Kind` + `classof`) | `AST.h`; used by `Parser::parseBinOpRHS` |
| Tests | `tests/LexerTests.cpp`, `tests/RunJit.cmake`, `enable_testing()` in `CMakeLists.txt` |

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
./build/toy --dump-ast < tests/fib.ks    # print the parse tree
./build/toy -c tests/fib.ks --emit-llvm  # ...and print the module's IR
```

### Comparing against upstream

Several notes below cite the tutorial's `toy.cpp` for a given chapter. Those
sources are not vendored here -- publishing a copy of unmodified LLVM code
alongside this one adds nothing. Fetch them when you need the comparison:

```bash
git clone --depth 1 --branch release/20.x \
    https://github.com/llvm/llvm-project.git /tmp/llvm
ls /tmp/llvm/llvm/examples/Kaleidoscope/       # Chapter2 .. Chapter9
```

`release/20.x` is the branch to use, matching the LLVM this builds against.
Each chapter is a single self-contained file:

```bash
clang++ -g -O0 /tmp/llvm/llvm/examples/Kaleidoscope/Chapter7/toy.cpp \
    $(llvm-config --cxxflags --ldflags --system-libs --libs core orcjit native) \
    -o /tmp/ch7
```

`include/KaleidoscopeJIT.h` is the one upstream file kept in this repo, because
the build needs it. It is unmodified and carries its original LLVM license
header.
