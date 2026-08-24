#include "ASTDumper.h"
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
  bool Compile = false;  // -c: compile a file instead of running the REPL
  bool Debug = false;    // -g: emit DWARF debug info (implies -c)
  bool DumpAST = false;  // --dump-ast: print the parse tree
  bool EmitLLVM = false; // --emit-llvm: print the module's IR (-c only;
                         // the REPL always prints it at EOF)
  std::string Input;     // source file for -c
  std::string Output = "output.o";
};

void usage(const char *Prog) {
  errs() << "usage: " << Prog << " [-c <file.ks> [-g] [-o <file.o>]]\n"
         << "  no arguments   read stdin, JIT and evaluate interactively\n"
         << "  -c <file.ks>   compile to a native object file\n"
         << "  -g             emit debug info (disables optimization)\n"
         << "  -o <file.o>    output object name (default output.o)\n"
         << "  --dump-ast     print the AST for each construct\n"
         << "  --emit-llvm    with -c, also print the module's IR\n"
         << "                 (the REPL always prints it at EOF)\n";
}

/// Prints a whole module.
void emitModuleIR(CodeGen &CG) { CG.getModule().print(errs(), nullptr); }

/// Appends a module's IR to a buffer. Each module is handed to the JIT and a
/// fresh one opened, so nothing survives to EOF unless captured here.
void captureModuleIR(CodeGen &CG, std::string &Out) {
  if (CG.getModule().empty())
    return;
  raw_string_ostream OS(Out);
  CG.getModule().print(OS, nullptr);
}

//===----------------------------------------------------------------------===//
// Interactive JIT driver (Chapters 4-7)
//===----------------------------------------------------------------------===//

int runInteractive(const Options &O) {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  ExitOnError ExitOnErr;
  auto TheJIT = ExitOnErr(orc::KaleidoscopeJIT::Create());

  OperatorTable Ops;
  Lexer Lex(std::cin);
  CodeGen CG(Ops);
  ASTDumper Dumper(std::cout);
  CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), /*Optimize=*/true);

  fprintf(stderr, "ready> ");
  Parser P(Lex, Ops); // primes the first token

  // IR of every module, shown together at EOF.
  std::string AllIR;

  while (true) {
    switch (P.getCurTok()) {
    case tok_eof:
      // The working module was never handed off; it holds anything written
      // since the last definition, externs included.
      captureModuleIR(CG, AllIR);
      if (!AllIR.empty())
        errs() << "\n" << AllIR;
      return 0;

    case ';':
      // Ignore top-level semicolons, and print no prompt for them.
      P.advance();
      continue;

    case tok_def:
      if (auto FnAST = P.parseDefinition()) {
        if (O.DumpAST)
          Dumper.dump(*FnAST);
        if (Function *FnIR = CG.codegen(*FnAST)) {
          fprintf(stderr, "Read function definition:");
          FnIR->print(errs());
          fprintf(stderr, "\n");

          captureModuleIR(CG, AllIR);

          auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
          ExitOnErr(TheJIT->addModule(std::move(TSM)));
          CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(),
                        /*Optimize=*/true);
        }
      } else {
        P.advance(); // error recovery
      }
      break;

    case tok_extern:
      if (auto ProtoAST = P.parseExtern()) {
        if (O.DumpAST)
          Dumper.dump(*ProtoAST);
        if (Function *FnIR = CG.codegen(*ProtoAST)) {
          fprintf(stderr, "Read extern: ");
          FnIR->print(errs());
          fprintf(stderr, "\n");
          CG.addPrototype(std::move(ProtoAST));
        }
      } else {
        P.advance();
      }
      break;

    default:
      if (auto FnAST = P.parseTopLevelExpr()) {
        if (O.DumpAST)
          Dumper.dump(*FnAST);
        if (CG.codegen(*FnAST)) {
          captureModuleIR(CG, AllIR);

          auto RT = TheJIT->getMainJITDylib().createResourceTracker();
          auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
          ExitOnErr(TheJIT->addModule(std::move(TSM), RT));

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
      break;
    }

    fprintf(stderr, "ready> ");
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
  ASTDumper Dumper(std::cout);
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
      if (FnAST && O.DumpAST)
        Dumper.dump(*FnAST);
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

    // A top-level expression becomes main(), so there can only be one.
    if (SawTopLevel) {
      errs() << "only one top-level expression is supported when compiling "
                "(it becomes main)\n";
      Failed = true;
      break;
    }
    auto FnAST = P.parseTopLevelExpr("main");
    if (FnAST && O.DumpAST)
      Dumper.dump(*FnAST);
    if (!FnAST || !CG.codegen(*FnAST)) {
      errs() << "error generating code for top level expr\n";
      Failed = true;
      if (!FnAST)
        P.advance();
      continue;
    }
    SawTopLevel = true;
  }

  // A rejected token still lexes to a usable value, so the parse can succeed
  // on input the lexer already complained about. Ask before writing a .o.
  if (Lex.hadError())
    Failed = true;

  if (O.Debug)
    DBuilder->finalize();

  if (O.EmitLLVM)
    emitModuleIR(CG);

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
    } else if (Arg == "--dump-ast") {
      O.DumpAST = true;
    } else if (Arg == "--emit-llvm") {
      O.EmitLLVM = true;
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

  return O.Compile ? runCompile(O) : runInteractive(O);
}
