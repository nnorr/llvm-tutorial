#ifndef KALEIDOSCOPE_DEBUGINFO_H
#define KALEIDOSCOPE_DEBUGINFO_H

#include "AST.h"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"

#include <vector>

namespace kaleidoscope {

class DebugInfo {
  llvm::DIBuilder &DBuilder;
  llvm::IRBuilder<> &Builder;
  llvm::DICompileUnit *TheCU;
  llvm::DIType *DblTy = nullptr;

  std::vector<llvm::DIScope *> LexicalBlocks;

public:
  DebugInfo(llvm::DIBuilder &DBuilder, llvm::IRBuilder<> &Builder,
            llvm::DICompileUnit *TheCU)
      : DBuilder(DBuilder), Builder(Builder), TheCU(TheCU) {}

  llvm::DICompileUnit *getCompileUnit() const { return TheCU; }

  llvm::DIType *getDoubleTy();

  void emitLocation(const ExprAST *AST);

  llvm::DISubroutineType *createFunctionType(unsigned NumArgs);

  llvm::DISubprogram *createFunction(llvm::StringRef Name, unsigned LineNo,
                                     unsigned NumArgs);

  void declareParameter(llvm::DISubprogram *SP, llvm::StringRef Name,
                        unsigned ArgIdx, unsigned LineNo,
                        llvm::AllocaInst *Alloca, llvm::BasicBlock *BB);

  void pushScope(llvm::DIScope *Scope) { LexicalBlocks.push_back(Scope); }
  void popScope() { LexicalBlocks.pop_back(); }
};

} // namespace kaleidoscope

#endif
