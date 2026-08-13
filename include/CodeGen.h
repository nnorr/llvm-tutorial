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

/// CodeGen - Lowers the AST to LLVM IR.
///
/// Implemented as an ASTVisitor rather than as codegen() methods on the nodes.
/// visit() cannot return llvm::Value* without the AST knowing about LLVM, so
/// the produced value is stashed in Result and read back by codegenExpr().
///
/// Module lifecycle: the JIT driver calls initModule() before every top-level
/// expression, hands the finished module to the JIT, and re-initializes. The
/// object-file driver calls it once and accumulates everything into a single
/// module. Hence this is NOT a construct-once object.
class CodeGen : public ASTVisitor {
  OperatorTable &Ops;

  /// Null unless debug info is being emitted. The JIT path leaves it null:
  /// DWARF describes a file on disk, which a REPL does not have.
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

  /// Variables in scope, as stack slots. mem2reg (PromotePass) turns these
  /// into SSA registers, which is why codegen never builds PHI nodes by hand
  /// for mutable variables.
  std::map<std::string, llvm::AllocaInst *> NamedValues;

  /// Prototypes seen so far, so a function can be re-declared into a fresh
  /// module after the previous one was handed to the JIT.
  std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

  /// Set by the visit() methods, read by codegenExpr().
  llvm::Value *Result = nullptr;

  llvm::Value *codegenExpr(ExprAST &E);
  llvm::Function *getFunction(const std::string &Name);
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Function *F,
                                           llvm::StringRef VarName);

  void visit(NumberExprAST &E) override;
  void visit(VariableExprAST &E) override;
  void visit(UnaryExprAST &E) override;
  void visit(BinaryExprAST &E) override;
  void visit(CallExprAST &E) override;
  void visit(IfExprAST &E) override;
  void visit(ForExprAST &E) override;
  void visit(VarExprAST &E) override;

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
