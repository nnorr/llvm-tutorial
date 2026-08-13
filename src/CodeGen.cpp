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

  // Open a new context and module.
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

  // Promote allocas to registers -- this is what lets ForExprAST/VarExprAST
  // emit plain stack slots instead of hand-built PHI nodes.
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

Value *CodeGen::codegenExpr(ExprAST &E) {
  Result = nullptr;
  E.accept(*this);
  return Result;
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
  // Always the entry block, never the current one -- mem2reg only promotes
  // allocas it finds there.
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(*Ctx), nullptr, VarName);
}

void CodeGen::visit(NumberExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);
  Result = ConstantFP::get(*Ctx, APFloat(E.getVal()));
}

void CodeGen::visit(VariableExprAST &E) {
  // Look this variable up in the function.
  AllocaInst *V = NamedValues[E.getName()];
  if (!V) {
    Result = logErrorV("Unknown variable name");
    return;
  }

  if (Dbg)
    Dbg->emitLocation(&E);
  Result = Builder->CreateLoad(Type::getDoubleTy(*Ctx), V, E.getName().c_str());
}

void CodeGen::visit(UnaryExprAST &E) {
  Value *OperandV = codegenExpr(E.getOperand());
  if (!OperandV) {
    Result = nullptr;
    return;
  }

  Function *F = getFunction(std::string("unary") + E.getOpcode());
  if (!F) {
    Result = logErrorV("Unknown unary operator");
    return;
  }

  if (Dbg)
    Dbg->emitLocation(&E);
  Result = Builder->CreateCall(F, OperandV, "unop");
}

void CodeGen::visit(BinaryExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  // Special case '=' because we don't want to emit the LHS as an expression.
  if (E.getOp() == '=') {
    // The tutorial uses static_cast here (LLVM is usually built -fno-rtti) and
    // then null-checks the result, which is dead code -- static_cast never
    // yields null. dynamic_cast actually performs the check.
    auto *LHSE = dynamic_cast<VariableExprAST *>(&E.getLHS());
    if (!LHSE) {
      Result = logErrorV("destination of '=' must be a variable");
      return;
    }

    Value *Val = codegenExpr(E.getRHS());
    if (!Val) {
      Result = nullptr;
      return;
    }

    Value *Variable = NamedValues[LHSE->getName()];
    if (!Variable) {
      Result = logErrorV("Unknown variable name");
      return;
    }

    Builder->CreateStore(Val, Variable);
    Result = Val;
    return;
  }

  Value *L = codegenExpr(E.getLHS());
  Value *R = codegenExpr(E.getRHS());
  if (!L || !R) {
    Result = nullptr;
    return;
  }

  switch (E.getOp()) {
  case '+':
    Result = Builder->CreateFAdd(L, R, "addtmp");
    return;
  case '-':
    Result = Builder->CreateFSub(L, R, "subtmp");
    return;
  case '*':
    Result = Builder->CreateFMul(L, R, "multmp");
    return;
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    Result = Builder->CreateUIToFP(L, Type::getDoubleTy(*Ctx), "booltmp");
    return;
  default:
    break;
  }

  // If it wasn't a builtin binary operator, it must be a user defined one.
  Function *F = getFunction(std::string("binary") + E.getOp());
  if (!F) {
    Result = logErrorV("binary operator not found");
    return;
  }

  Value *Ops2[] = {L, R};
  Result = Builder->CreateCall(F, Ops2, "binop");
}

void CodeGen::visit(CallExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  // Look up the name in the global module table.
  Function *CalleeF = getFunction(E.getCallee());
  if (!CalleeF) {
    Result = logErrorV("Unknown function referenced");
    return;
  }

  if (CalleeF->arg_size() != E.getArgs().size()) {
    Result = logErrorV("Incorrect # arguments passed");
    return;
  }

  std::vector<Value *> ArgsV;
  for (const auto &Arg : E.getArgs()) {
    ArgsV.push_back(codegenExpr(*Arg));
    if (!ArgsV.back()) {
      Result = nullptr;
      return;
    }
  }

  Result = Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

void CodeGen::visit(IfExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);

  Value *CondV = codegenExpr(E.getCond());
  if (!CondV) {
    Result = nullptr;
    return;
  }

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
  Value *ThenV = codegenExpr(E.getThen());
  if (!ThenV) {
    Result = nullptr;
    return;
  }
  Builder->CreateBr(MergeBB);
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = Builder->GetInsertBlock();

  // Emit else block.
  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = codegenExpr(E.getElse());
  if (!ElseV) {
    Result = nullptr;
    return;
  }
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  // Emit merge block.
  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*Ctx), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  Result = PN;
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
/// NOTE: this is a do-while -- the body always runs at least once, and the end
/// condition is evaluated *before* the induction variable is incremented.
void CodeGen::visit(ForExprAST &E) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Create an alloca for the variable in the entry block.
  AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, E.getVarName());

  if (Dbg)
    Dbg->emitLocation(&E);

  // Emit the start code first, without 'variable' in scope.
  Value *StartVal = codegenExpr(E.getStart());
  if (!StartVal) {
    Result = nullptr;
    return;
  }
  Builder->CreateStore(StartVal, Alloca);

  BasicBlock *LoopBB = BasicBlock::Create(*Ctx, "loop", TheFunction);
  Builder->CreateBr(LoopBB);
  Builder->SetInsertPoint(LoopBB);

  // Within the loop the variable shadows any existing one; save it to restore.
  AllocaInst *OldVal = NamedValues[E.getVarName()];
  NamedValues[E.getVarName()] = Alloca;

  // Emit the body. Its value is ignored, but an error is not allowed.
  if (!codegenExpr(E.getBody())) {
    Result = nullptr;
    return;
  }

  // Emit the step value, defaulting to 1.0.
  Value *StepVal = nullptr;
  if (ExprAST *Step = E.getStep()) {
    StepVal = codegenExpr(*Step);
    if (!StepVal) {
      Result = nullptr;
      return;
    }
  } else {
    StepVal = ConstantFP::get(*Ctx, APFloat(1.0));
  }

  Value *EndCond = codegenExpr(E.getEnd());
  if (!EndCond) {
    Result = nullptr;
    return;
  }

  // Reload, increment, and restore the alloca. This handles the case where the
  // body of the loop mutates the variable.
  Value *CurVar = Builder->CreateLoad(Type::getDoubleTy(*Ctx), Alloca,
                                      E.getVarName().c_str());
  Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
  Builder->CreateStore(NextVar, Alloca);

  EndCond = Builder->CreateFCmpONE(EndCond, ConstantFP::get(*Ctx, APFloat(0.0)),
                                   "loopcond");

  BasicBlock *AfterBB = BasicBlock::Create(*Ctx, "afterloop", TheFunction);
  Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // Restore the unshadowed variable.
  if (OldVal)
    NamedValues[E.getVarName()] = OldVal;
  else
    NamedValues.erase(E.getVarName());

  // for expr always returns 0.0.
  Result = Constant::getNullValue(Type::getDoubleTy(*Ctx));
}

void CodeGen::visit(VarExprAST &E) {
  std::vector<AllocaInst *> OldBindings;

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Register all variables and emit their initializer.
  for (const auto &NamedVar : E.getVarNames()) {
    const std::string &VarName = NamedVar.first;
    ExprAST *Init = NamedVar.second.get();

    // Emit the initializer before adding the variable to scope, so that
    //   var a = 1 in var a = a in ...
    // refers to the outer 'a'.
    Value *InitVal;
    if (Init) {
      InitVal = codegenExpr(*Init);
      if (!InitVal) {
        Result = nullptr;
        return;
      }
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

  // Codegen the body, now that all vars are in scope.
  Value *BodyVal = codegenExpr(E.getBody());
  if (!BodyVal) {
    Result = nullptr;
    return;
  }

  // Pop all our variables from scope.
  unsigned Idx = 0;
  for (const auto &NamedVar : E.getVarNames())
    NamedValues[NamedVar.first] = OldBindings[Idx++];

  Result = BodyVal;
}

Function *CodeGen::codegen(PrototypeAST &P) {
  // Make the function type: double(double, double) etc.
  std::vector<Type *> Doubles(P.getArgs().size(), Type::getDoubleTy(*Ctx));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(*Ctx), Doubles, false);

  Function *F =
      Function::Create(FT, Function::ExternalLinkage, P.getName(), Mod.get());

  // Set names for all arguments.
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(P.getArgs()[Idx++]);

  return F;
}

Function *CodeGen::codegen(FunctionAST &F) {
  // Take ownership of the prototype so it can be re-declared into a later
  // module, but keep a reference for use below.
  PrototypeAST &P = F.getProto();
  FunctionProtos[P.getName()] = F.takeProto();

  Function *TheFunction = getFunction(P.getName());
  if (!TheFunction)
    return nullptr;

  // If this is an operator, install it so the parser can use it from here on.
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

    // Unset the location for the prologue: leading instructions with no
    // location are treated as prologue, and the debugger steps past them.
    Dbg->emitLocation(nullptr);
  }

  // Record the function arguments in the NamedValues map.
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

  if (Value *RetVal = codegenExpr(F.getBody())) {
    Builder->CreateRet(RetVal);

    if (Dbg)
      Dbg->popScope();

    // Validate the generated code, checking for consistency.
    verifyFunction(*TheFunction);

    if (OptimizeFunctions && FPM)
      FPM->run(*TheFunction, *FAM);

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();

  // NOTE: the tutorial reads Proto->getOperatorName() here, but Proto was
  // moved out above, so it dereferences a null unique_ptr. Use the reference.
  if (P.isBinaryOp())
    Ops.erase(P.getOperatorName());

  if (Dbg)
    Dbg->popScope();

  return nullptr;
}

} // namespace kaleidoscope
