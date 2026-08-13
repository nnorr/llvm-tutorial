#ifndef KALEIDOSCOPE_DEBUGINFO_H
#define KALEIDOSCOPE_DEBUGINFO_H

#include "AST.h"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"

#include <vector>

namespace kaleidoscope {

/// DebugInfo - Emits DWARF metadata alongside the generated IR.
///
/// In the tutorial this was a global `KSDbgInfo` struct reaching for the global
/// DBuilder and Builder. Here it holds explicit references, so the ownership of
/// the DIBuilder (which needs the Module to already exist) is visible.
class DebugInfo {
  llvm::DIBuilder &DBuilder;
  llvm::IRBuilder<> &Builder;
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy = nullptr;

  /// Stack of enclosing scopes; the back() is the innermost.
  std::vector<llvm::DIScope *> LexicalBlocks;

public:
  DebugInfo(llvm::DIBuilder &DBuilder, llvm::IRBuilder<> &Builder,
            llvm::DICompileUnit *TheCU)
      : DBuilder(DBuilder), Builder(Builder), TheCU(TheCU) {}

  llvm::DICompileUnit *getCompileUnit() const { return TheCU; }

  /// Kaleidoscope has exactly one type, so this is cached.
  llvm::DIType *getDoubleTy();

  /// Sets the IRBuilder's current debug location from a node's source
  /// location. Passing null clears it -- used to keep the function prologue
  /// unattributed so debuggers step past it.
  void emitLocation(const ExprAST *AST);

  /// double(double, double, ...) with NumArgs parameters.
  llvm::DISubroutineType *createFunctionType(unsigned NumArgs);

  /// Creates the subprogram DIE for a function definition.
  llvm::DISubprogram *createFunction(llvm::StringRef Name, unsigned LineNo,
                                     unsigned NumArgs);

  /// Attaches a parameter variable DIE to \p Alloca. ArgIdx is 1-based.
  void declareParameter(llvm::DISubprogram *SP, llvm::StringRef Name,
                        unsigned ArgIdx, unsigned LineNo,
                        llvm::AllocaInst *Alloca, llvm::BasicBlock *BB);

  void pushScope(llvm::DIScope *Scope) { LexicalBlocks.push_back(Scope); }
  void popScope() { LexicalBlocks.pop_back(); }
};

} // namespace kaleidoscope

#endif
