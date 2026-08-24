#ifndef KALEIDOSCOPE_CODEGEN_H
#define KALEIDOSCOPE_CODEGEN_H

#include "AST.h"
#include "ASTVisitor.h"
#include "DebugInfo.h"
#include "OperatorTable.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/StandardInstrumentations.h"

#include <map>
#include <memory>
#include <string>

namespace kaleidoscope {

/// CodeGen - Lowers the AST to LLVM IR. An ASTVisitor with RetTy = Value*.
///
/// Not a construct-once object. The JIT driver calls initModule() before every
/// top-level expression and hands the finished module away; the object-file
/// driver calls it once and accumulates everything into a single module.
class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> {
  friend class ASTVisitor<CodeGen, llvm::Value *>; // calls the hooks below

  OperatorTable &Ops;

  /// Null unless -g. The JIT path leaves it null: DWARF describes a file on
  /// disk, which a REPL does not have.
  DebugInfo *Dbg = nullptr;

  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  std::unique_ptr<llvm::IRBuilder<>> Builder;

  /// New-style pass manager pipeline, rebuilt with every module.
  bool OptimizeFunctions = true;
  std::unique_ptr<llvm::FunctionPassManager> FPM;
  std::unique_ptr<llvm::LoopAnalysisManager> LAM;
  std::unique_ptr<llvm::FunctionAnalysisManager> FAM;
  std::unique_ptr<llvm::CGSCCAnalysisManager> CGAM;
  std::unique_ptr<llvm::ModuleAnalysisManager> MAM;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> PIC;
  std::unique_ptr<llvm::StandardInstrumentations> SI;

  /// Variables in scope, as stack slots. mem2reg promotes them to SSA
  /// registers, so codegen never builds PHI nodes for mutable variables.
  std::map<std::string, llvm::AllocaInst *> NamedValues;

  /// Prototypes seen so far, for re-declaring into a fresh module.
  std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

  llvm::Function *getFunction(const std::string &Name);
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Function *F,
                                           llvm::StringRef VarName);

  /// Per-node hooks, reached through the inherited visit(ExprAST &). Each
  /// returns null on error, having already reported it.
  llvm::Value *visitNumber(NumberExprAST &E);
  llvm::Value *visitVariable(VariableExprAST &E);
  llvm::Value *visitUnary(UnaryExprAST &E);
  llvm::Value *visitBinary(BinaryExprAST &E);
  llvm::Value *visitAssign(AssignExprAST &E);
  llvm::Value *visitCall(CallExprAST &E);
  llvm::Value *visitIf(IfExprAST &E);
  llvm::Value *visitFor(ForExprAST &E);
  llvm::Value *visitVar(VarExprAST &E);

public:
  explicit CodeGen(OperatorTable &Ops) : Ops(Ops) {}

  /// Opens a fresh context/module/builder and rebuilds the pass pipeline.
  /// Must be called before any codegen.
  void initModule(llvm::StringRef ModuleName, const llvm::DataLayout &DL,
                  bool Optimize);

  /// Debug info needs the Module and IRBuilder, so it can only be constructed
  /// after initModule(). Pass null to disable.
  void setDebugInfo(DebugInfo *D) { Dbg = D; }

  llvm::LLVMContext &getContext() { return *Ctx; }
  llvm::Module &getModule() { return *Mod; }
  llvm::IRBuilder<> &getBuilder() { return *Builder; }

  /// Relinquish the module/context, e.g. to wrap in a ThreadSafeModule for the
  /// JIT. initModule() must be called again before further codegen.
  std::unique_ptr<llvm::Module> takeModule() { return std::move(Mod); }
  std::unique_ptr<llvm::LLVMContext> takeContext() { return std::move(Ctx); }

  /// Records a prototype (from an 'extern') so later modules can re-declare it.
  void addPrototype(std::unique_ptr<PrototypeAST> Proto);

  llvm::Function *codegen(PrototypeAST &P);
  llvm::Function *codegen(FunctionAST &F);
};

} // namespace kaleidoscope

#endif
