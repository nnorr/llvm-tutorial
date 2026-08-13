#include "DebugInfo.h"

#include "llvm/BinaryFormat/Dwarf.h"

using namespace llvm;

namespace kaleidoscope {

DIType *DebugInfo::getDoubleTy() {
  if (DblTy)
    return DblTy;

  DblTy = DBuilder.createBasicType("double", 64, dwarf::DW_ATE_float);
  return DblTy;
}

void DebugInfo::emitLocation(const ExprAST *AST) {
  if (!AST)
    return Builder.SetCurrentDebugLocation(DebugLoc());

  DIScope *Scope;
  if (LexicalBlocks.empty())
    Scope = TheCU;
  else
    Scope = LexicalBlocks.back();

  Builder.SetCurrentDebugLocation(DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

DISubprogram *DebugInfo::createFunction(StringRef Name, unsigned LineNo,
                                        unsigned NumArgs) {
  DIFile *Unit =
      DBuilder.createFile(TheCU->getFilename(), TheCU->getDirectory());
  DIScope *FContext = Unit;
  unsigned ScopeLine = LineNo;
  return DBuilder.createFunction(
      FContext, Name, StringRef(), Unit, LineNo, createFunctionType(NumArgs),
      ScopeLine, DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
}

void DebugInfo::declareParameter(DISubprogram *SP, StringRef Name,
                                 unsigned ArgIdx, unsigned LineNo,
                                 AllocaInst *Alloca, BasicBlock *BB) {
  DIFile *Unit =
      DBuilder.createFile(TheCU->getFilename(), TheCU->getDirectory());
  DILocalVariable *D = DBuilder.createParameterVariable(
      SP, Name, ArgIdx, Unit, LineNo, getDoubleTy(), true);

  DBuilder.insertDeclare(Alloca, D, DBuilder.createExpression(),
                         DILocation::get(SP->getContext(), LineNo, 0, SP), BB);
}

DISubroutineType *DebugInfo::createFunctionType(unsigned NumArgs) {
  SmallVector<Metadata *, 8> EltTys;
  DIType *DblTy = getDoubleTy();

  // Add the result type.
  EltTys.push_back(DblTy);

  for (unsigned I = 0; I != NumArgs; ++I)
    EltTys.push_back(DblTy);

  return DBuilder.createSubroutineType(DBuilder.getOrCreateTypeArray(EltTys));
}

} // namespace kaleidoscope
