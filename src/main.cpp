#include "CodeGen.h"
#include "DebugInfo.h"
#include "KaleidoscopeJIT.h"
#include "Lexer.h"
#include "ObjectEmitter.h"
#include "OperatorTable.h"
#include "Parser.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace llvm;
using namespace kaleidoscope;

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc(static_cast<char>(X), stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

namespace {

struct Options {
  bool Compile = false;      // -c: compile a file instead of running the REPL
  bool Debug = false;        // -g: emit DWARF debug info (implies -c)
  std::string Input;         // source file for -c
  std::string Output = "output.o";
};

void usage(const char *Prog) {
  errs() << "usage: " << Prog << " [-c <file.ks> [-g] [-o <file.o>]]\n"
         << "  no arguments   read stdin, JIT and evaluate interactively\n"
         << "  -c <file.ks>   compile to a native object file\n"
         << "  -g             emit debug info (disables optimization)\n"
         << "  -o <file.o>    output object name (default output.o)\n";
}

//===----------------------------------------------------------------------===//
// Interactive JIT driver (Chapters 4-7)
//===----------------------------------------------------------------------===//

int runInteractive() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  ExitOnError ExitOnErr;
  auto TheJIT = ExitOnErr(orc::KaleidoscopeJIT::Create());

  OperatorTable Ops;
  Lexer Lex(std::cin);
  CodeGen CG(Ops);
  CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), /*Optimize=*/true);

  fprintf(stderr, "ready> ");
  Parser P(Lex, Ops); // primes the first token

  while (true) {
    switch (P.getCurTok()) {
    case tok_eof:
      return 0;

    case ';': // ignore top-level semicolons.
      P.advance();
      break;

    case tok_def:
      if (auto FnAST = P.parseDefinition()) {
        if (Function *FnIR = CG.codegen(*FnAST)) {
          fprintf(stderr, "Read function definition:");
          FnIR->print(errs());
          fprintf(stderr, "\n");

          // Hand the definition to the JIT right away, in its own module and
          // with NO ResourceTracker so it persists. If it were left in the
          // working module it would be swept away with the next top-level
          // expression, whose module *is* tracked and removed after
          // evaluation -- and later calls would fail to resolve the symbol.
          auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
          ExitOnErr(TheJIT->addModule(std::move(TSM)));
          CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(),
                        /*Optimize=*/true);
        }
      } else {
        P.advance(); // error recovery
      }
      fprintf(stderr, "ready> ");
      break;

    case tok_extern:
      if (auto ProtoAST = P.parseExtern()) {
        if (Function *FnIR = CG.codegen(*ProtoAST)) {
          fprintf(stderr, "Read extern: ");
          FnIR->print(errs());
          fprintf(stderr, "\n");
          CG.addPrototype(std::move(ProtoAST));
        }
      } else {
        P.advance();
      }
      fprintf(stderr, "ready> ");
      break;

    default:
      if (auto FnAST = P.parseTopLevelExpr()) {
        if (CG.codegen(*FnAST)) {
          // Track the JIT'd memory for this anonymous expression so it can be
          // freed once evaluated.
          auto RT = TheJIT->getMainJITDylib().createResourceTracker();
          auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
          ExitOnErr(TheJIT->addModule(std::move(TSM), RT));

          // The module was handed away -- open a fresh one before continuing.
          CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(),
                        /*Optimize=*/true);

          auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
          double (*FP)() = ExprSymbol.toPtr<double (*)()>();
          fprintf(stderr, "Evaluated to %f\n", FP());

          ExitOnErr(RT->remove());
        }
      } else {
        P.advance();
      }
      fprintf(stderr, "ready> ");
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// Object-file driver (Chapter 8) with optional debug info (Chapter 9)
//===----------------------------------------------------------------------===//

int runCompile(const Options &O) {
  ObjectEmitter::initializeTargets();

  std::string Error;
  auto TM = ObjectEmitter::createHostTargetMachine(Error);
  if (!TM) {
    errs() << Error << "\n";
    return 1;
  }

  std::ifstream In(O.Input);
  if (!In) {
    errs() << "could not open " << O.Input << "\n";
    return 1;
  }

  OperatorTable Ops;
  Lexer Lex(In);
  CodeGen CG(Ops);
  // Optimization is disabled with -g: the Ch9 pipeline emits no passes, and
  // optimized code makes the line tables much harder to follow in a debugger.
  CG.initModule(O.Input, TM->createDataLayout(), /*Optimize=*/!O.Debug);

  std::unique_ptr<DIBuilder> DBuilder;
  std::unique_ptr<DebugInfo> Dbg;
  if (O.Debug) {
    Module &M = CG.getModule();
    M.addModuleFlag(Module::Warning, "Debug Info Version",
                    DEBUG_METADATA_VERSION);

    DBuilder = std::make_unique<DIBuilder>(M);
    DICompileUnit *CU = DBuilder->createCompileUnit(
        dwarf::DW_LANG_C, DBuilder->createFile(O.Input, "."),
        "Kaleidoscope Compiler", /*isOptimized=*/false, "", 0);

    Dbg = std::make_unique<DebugInfo>(*DBuilder, CG.getBuilder(), CU);
    CG.setDebugInfo(Dbg.get());
  }

  Parser P(Lex, Ops);
  bool SawTopLevel = false;
  bool Failed = false;

  while (true) {
    int Tok = P.getCurTok();
    if (Tok == tok_eof)
      break;

    if (Tok == ';') {
      P.advance();
      continue;
    }

    if (Tok == tok_def) {
      auto FnAST = P.parseDefinition();
      if (!FnAST || !CG.codegen(*FnAST)) {
        errs() << "error reading function definition\n";
        Failed = true;
        if (!FnAST)
          P.advance();
      }
      continue;
    }

    if (Tok == tok_extern) {
      auto ProtoAST = P.parseExtern();
      if (!ProtoAST || !CG.codegen(*ProtoAST)) {
        errs() << "error reading extern\n";
        Failed = true;
        if (!ProtoAST)
          P.advance();
        continue;
      }
      CG.addPrototype(std::move(ProtoAST));
      continue;
    }

    // A top-level expression becomes main(), so there can only be one. This is
    // the same restriction the tutorial notes in Chapter 8.
    if (SawTopLevel) {
      errs() << "only one top-level expression is supported when compiling "
                "(it becomes main)\n";
      Failed = true;
      break;
    }
    auto FnAST = P.parseTopLevelExpr("main");
    if (!FnAST || !CG.codegen(*FnAST)) {
      errs() << "error generating code for top level expr\n";
      Failed = true;
      if (!FnAST)
        P.advance();
      continue;
    }
    SawTopLevel = true;
  }

  if (O.Debug)
    DBuilder->finalize();

  if (Failed)
    return 1;

  if (!ObjectEmitter::emit(CG.getModule(), *TM, O.Output, Error)) {
    errs() << Error << "\n";
    return 1;
  }

  outs() << "Wrote " << O.Output << "\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Options O;

  for (int I = 1; I < argc; ++I) {
    std::string Arg = argv[I];
    if (Arg == "-c" && I + 1 < argc) {
      O.Compile = true;
      O.Input = argv[++I];
    } else if (Arg == "-o" && I + 1 < argc) {
      O.Output = argv[++I];
    } else if (Arg == "-g") {
      O.Debug = true;
    } else if (Arg == "-h" || Arg == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      errs() << "unknown argument: " << Arg << "\n";
      usage(argv[0]);
      return 1;
    }
  }

  if (O.Debug && !O.Compile) {
    errs() << "-g requires -c: debug info describes a source file, which the "
              "interactive JIT does not have\n";
    return 1;
  }

  return O.Compile ? runCompile(O) : runInteractive();
}
