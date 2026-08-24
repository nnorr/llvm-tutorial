#include "CodeGen.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <cstdio>
#include <vector>

using namespace llvm;

namespace kaleidoscope {

namespace {

Value *logErrorV(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

} // namespace

void CodeGen::initModule(StringRef ModuleName, const DataLayout &DL,
                         bool Optimize) {
  OptimizeFunctions = Optimize;

  Ctx = std::make_unique<LLVMContext>();
  Mod = std::make_unique<Module>(ModuleName, *Ctx);
  Mod->setDataLayout(DL);

  Builder = std::make_unique<IRBuilder<>>(*Ctx);

  // A new module means new analysis state, so the managers are rebuilt too.
  FPM = std::make_unique<FunctionPassManager>();
  LAM = std::make_unique<LoopAnalysisManager>();
  FAM = std::make_unique<FunctionAnalysisManager>();
  CGAM = std::make_unique<CGSCCAnalysisManager>();
  MAM = std::make_unique<ModuleAnalysisManager>();
  PIC = std::make_unique<PassInstrumentationCallbacks>();
  SI = std::make_unique<StandardInstrumentations>(*Ctx, /*DebugLogging*/ false);
  SI->registerCallbacks(*PIC, MAM.get());

  // Promotes the stack slots ForExprAST/VarExprAST emit into SSA registers.
  FPM->addPass(PromotePass());
  // Simple "peephole" optimizations and bit-twiddling optzns.
  FPM->addPass(InstCombinePass());
  // Reassociate expressions.
  FPM->addPass(ReassociatePass());
  // Eliminate common subexpressions.
  FPM->addPass(GVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  FPM->addPass(SimplifyCFGPass());

  PassBuilder PB;
  PB.registerModuleAnalyses(*MAM);
  PB.registerFunctionAnalyses(*FAM);
  PB.crossRegisterProxies(*LAM, *FAM, *CGAM, *MAM);
}

void CodeGen::addPrototype(std::unique_ptr<PrototypeAST> Proto) {
  FunctionProtos[Proto->getName()] = std::move(Proto);
}

Function *CodeGen::getFunction(const std::string &Name) {
  // First, see if the function has already been added to the current module.
  if (auto *F = Mod->getFunction(Name))
    return F;

  // If not, check whether we can codegen the declaration from some existing
  // prototype.
  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return codegen(*FI->second);

  // If no existing prototype exists, return null.
  return nullptr;
}

AllocaInst *CodeGen::createEntryBlockAlloca(Function *TheFunction,
                                            StringRef VarName) {
  // Always the entry block, never the current one: mem2reg only promotes
  // allocas it finds there.
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(*Ctx), nullptr, VarName);
}

Value *CodeGen::visitNumber(NumberExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);
  return ConstantFP::get(*Ctx, APFloat(E.getVal()));
}

Value *CodeGen::visitVariable(VariableExprAST &E) {
  // Look this variable up in the function.
  AllocaInst *V = NamedValues[E.getName()];
  if (!V)
    return logErrorV("Unknown variable name");

  if (Dbg)
    Dbg->emitLocation(&E);
  return Builder->CreateLoad(Type::getDoubleTy(*Ctx), V, E.getName().c_str());
}

Value *CodeGen::visitUnary(UnaryExprAST &E) {
  Value *OperandV = visit(E.getOperand());
  if (!OperandV)
    return nullptr;

  Function *F = getFunction(std::string("unary") + E.getOpcode());
  if (!F)
    return logErrorV("Unknown unary operator");

  if (Dbg)
    Dbg->emitLocation(&E);
  return Builder->CreateCall(F, OperandV, "unop");
}

/// The parser has already confirmed the destination is an identifier, so there
/// is no cast and no error path here.
Value *CodeGen::visitAssign(AssignExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  Value *Val = visit(E.getValue());
  if (!Val)
    return nullptr;

  AllocaInst *Variable = NamedValues[E.getName()];
  if (!Variable)
    return logErrorV("Unknown variable name");

  Builder->CreateStore(Val, Variable);
  return Val;
}

Value *CodeGen::visitBinary(BinaryExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  Value *L = visit(E.getLHS());
  Value *R = visit(E.getRHS());
  if (!L || !R)
    return nullptr;

  switch (E.getOp()) {
  case '+':
    return Builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder->CreateFSub(L, R, "subtmp");
  case '*':
    return Builder->CreateFMul(L, R, "multmp");
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return Builder->CreateUIToFP(L, Type::getDoubleTy(*Ctx), "booltmp");
  default:
    break;
  }

  // If it wasn't a builtin binary operator, it must be a user defined one.
  Function *F = getFunction(std::string("binary") + E.getOp());
  if (!F)
    return logErrorV("binary operator not found");

  Value *Ops2[] = {L, R};
  return Builder->CreateCall(F, Ops2, "binop");
}

Value *CodeGen::visitCall(CallExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  // Look up the name in the global module table.
  Function *CalleeF = getFunction(E.getCallee());
  if (!CalleeF)
    return logErrorV("Unknown function referenced");

  if (CalleeF->arg_size() != E.getArgs().size())
    return logErrorV("Incorrect # arguments passed");

  std::vector<Value *> ArgsV;
  for (const auto &Arg : E.getArgs()) {
    ArgsV.push_back(visit(*Arg));
    if (!ArgsV.back())
      return nullptr;
  }

  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Value *CodeGen::visitIf(IfExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  Value *CondV = visit(E.getCond());
  if (!CondV)
    return nullptr;

  // Convert condition to a bool by comparing non-equal to 0.0.
  CondV = Builder->CreateFCmpONE(CondV, ConstantFP::get(*Ctx, APFloat(0.0)),
                                 "ifcond");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  BasicBlock *ThenBB = BasicBlock::Create(*Ctx, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*Ctx, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*Ctx, "ifcont");

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // Emit then value.
  Builder->SetInsertPoint(ThenBB);
  Value *ThenV = visit(E.getThen());
  if (!ThenV)
    return nullptr;
  Builder->CreateBr(MergeBB);
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = Builder->GetInsertBlock();

  // Emit else block.
  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = visit(E.getElse());
  if (!ElseV)
    return nullptr;
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  // Emit merge block.
  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*Ctx), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

/// Output for-loop as:
///   var = alloca double
///   start = startexpr
///   store start -> var
///   goto loop
/// loop:
///   bodyexpr
///   step = stepexpr
///   endcond = endexpr
///   curvar = load var
///   nextvar = curvar + step
///   store nextvar -> var
///   br endcond, loop, endloop
/// outloop:
///
/// this is a do-while -- the body always runs at least once, and the end
/// condition is evaluated *before* the induction variable is incremented.
Value *CodeGen::visitFor(ForExprAST &E) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Create an alloca for the variable in the entry block.
  AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, E.getVarName());

  if (Dbg)
    Dbg->emitLocation(&E);

  Value *StartVal = visit(E.getStart());
  if (!StartVal)
    return nullptr;
  Builder->CreateStore(StartVal, Alloca);

  BasicBlock *LoopBB = BasicBlock::Create(*Ctx, "loop", TheFunction);
  Builder->CreateBr(LoopBB);
  Builder->SetInsertPoint(LoopBB);

  AllocaInst *OldVal = NamedValues[E.getVarName()];
  NamedValues[E.getVarName()] = Alloca;

  if (!visit(E.getBody()))
    return nullptr;

  Value *StepVal = nullptr;
  if (ExprAST *Step = E.getStep()) {
    StepVal = visit(*Step);
    if (!StepVal)
      return nullptr;
  } else {
    StepVal = ConstantFP::get(*Ctx, APFloat(1.0));
  }

  Value *EndCond = visit(E.getEnd());
  if (!EndCond)
    return nullptr;

  Value *CurVar = Builder->CreateLoad(Type::getDoubleTy(*Ctx), Alloca,
                                      E.getVarName().c_str());
  Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
  Builder->CreateStore(NextVar, Alloca);

  EndCond = Builder->CreateFCmpONE(EndCond, ConstantFP::get(*Ctx, APFloat(0.0)),
                                   "loopcond");

  BasicBlock *AfterBB = BasicBlock::Create(*Ctx, "afterloop", TheFunction);
  Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  Builder->SetInsertPoint(AfterBB);

  if (OldVal)
    NamedValues[E.getVarName()] = OldVal;
  else
    NamedValues.erase(E.getVarName());

  return Constant::getNullValue(Type::getDoubleTy(*Ctx));
}

Value *CodeGen::visitVar(VarExprAST &E) {
  std::vector<AllocaInst *> OldBindings;

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  for (const auto &NamedVar : E.getVarNames()) {
    const std::string &VarName = NamedVar.first;
    ExprAST *Init = NamedVar.second.get();

    Value *InitVal;
    if (Init) {
      InitVal = visit(*Init);
      if (!InitVal)
        return nullptr;
    } else {
      InitVal = ConstantFP::get(*Ctx, APFloat(0.0));
    }

    AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, VarName);
    Builder->CreateStore(InitVal, Alloca);

    OldBindings.push_back(NamedValues[VarName]);
    NamedValues[VarName] = Alloca;
  }

  if (Dbg)
    Dbg->emitLocation(&E);

  Value *BodyVal = visit(E.getBody());
  if (!BodyVal)
    return nullptr;

  unsigned Idx = 0;
  for (const auto &NamedVar : E.getVarNames())
    NamedValues[NamedVar.first] = OldBindings[Idx++];

  return BodyVal;
}

Function *CodeGen::codegen(PrototypeAST &P) {
  std::vector<Type *> Doubles(P.getArgs().size(), Type::getDoubleTy(*Ctx));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(*Ctx), Doubles, false);

  Function *F =
      Function::Create(FT, Function::ExternalLinkage, P.getName(), Mod.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(P.getArgs()[Idx++]);

  return F;
}

Function *CodeGen::codegen(FunctionAST &F) {
  PrototypeAST &P = F.getProto();
  FunctionProtos[P.getName()] = F.takeProto();

  Function *TheFunction = getFunction(P.getName());
  if (!TheFunction)
    return nullptr;

  if (P.isBinaryOp())
    Ops.setPrecedence(P.getOperatorName(), P.getBinaryPrecedence());

  BasicBlock *BB = BasicBlock::Create(*Ctx, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  DISubprogram *SP = nullptr;
  unsigned LineNo = static_cast<unsigned>(P.getLine());
  if (Dbg) {
    SP = Dbg->createFunction(P.getName(), LineNo, TheFunction->arg_size());
    TheFunction->setSubprogram(SP);
    Dbg->pushScope(SP);

    Dbg->emitLocation(nullptr);
  }

  NamedValues.clear();
  unsigned ArgIdx = 0;
  for (auto &Arg : TheFunction->args()) {
    AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, Arg.getName());

    if (Dbg)
      Dbg->declareParameter(SP, Arg.getName(), ++ArgIdx, LineNo, Alloca,
                            Builder->GetInsertBlock());

    Builder->CreateStore(&Arg, Alloca);
    NamedValues[std::string(Arg.getName())] = Alloca;
  }

  if (Dbg)
    Dbg->emitLocation(&F.getBody());

  if (Value *RetVal = visit(F.getBody())) {
    Builder->CreateRet(RetVal);

    if (Dbg)
      Dbg->popScope();

    verifyFunction(*TheFunction);

    if (OptimizeFunctions && FPM)
      FPM->run(*TheFunction, *FAM);

    return TheFunction;
  }

  TheFunction->eraseFromParent();

  if (P.isBinaryOp())
    Ops.erase(P.getOperatorName());

  if (Dbg)
    Dbg->popScope();

  return nullptr;
}

} // namespace kaleidoscope
