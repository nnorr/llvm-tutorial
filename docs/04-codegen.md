# 04. CodeGen — 트리를 LLVM IR로

**파일**: `include/CodeGen.h`, `src/CodeGen.cpp`
**원본 튜토리얼**: Chapter 3 (IR 생성), Chapter 4 (JIT·최적화), Chapter 5 (제어 흐름), Chapter 6 (연산자), Chapter 7 (변수)

가장 큰 모듈입니다 (약 620줄). LLVM 개념이 처음 나오므로 그것부터 정리합니다.

---

## 1. LLVM IR 기초

### 1.1 IR이란

LLVM **I**ntermediate **R**epresentation — 소스 언어와 기계어 사이의 중간 언어입니다.
어셈블리와 비슷하지만 기계 독립적이고, 타입이 있습니다.

```llvm
define double @add2(double %x) {
entry:
  %addtmp = fadd double %x, 2.000000e+00
  ret double %addtmp
}
```

- `define double @add2(double %x)` — `double`을 받아 `double`을 돌려주는 함수
- `entry:` — **기본 블록(basic block)** 의 이름표
- `%addtmp` — 계산 결과를 담는 이름. `@`는 전역, `%`는 지역
- `fadd` — 부동소수점 덧셈 (`f` = floating point)

Kaleidoscope는 타입이 `double` 하나뿐이라 IR도 단순합니다.

### 1.2 SSA — 한 번만 대입

LLVM IR의 가장 중요한 규칙: **모든 이름은 딱 한 번만 값이 정해집니다.**
Static Single Assignment의 약자입니다.

```llvm
%a = fadd double 1.0, 2.0
%a = fadd double 3.0, 4.0     ; ✗ 불가능. %a는 이미 정해졌음
```

값을 바꾸려면 새 이름을 써야 합니다. 그래서 **변수 대입을 어떻게 표현하나**
하는 문제가 생기고, 그 해답이 이 문서 5절(`alloca` + mem2reg)입니다.

### 1.3 기본 블록과 제어 흐름

**기본 블록**은 "중간에 끼어들거나 빠져나갈 수 없는 명령어 묶음"입니다.
반드시 분기 명령(`br`, `ret`)으로 끝납니다.

```llvm
entry:
  %cond = fcmp one double %x, 0.0
  br i1 %cond, label %then, label %else    ; 조건 분기

then:
  br label %ifcont                          ; 무조건 분기

else:
  br label %ifcont

ifcont:
  %result = phi double [ 1.0, %then ], [ 2.0, %else ]
```

마지막 `phi`가 SSA의 핵심 도구입니다. **"어느 블록에서 왔느냐에 따라 값이
정해진다"** 는 뜻입니다. `then`에서 왔으면 `1.0`, `else`에서 왔으면 `2.0`.

### 1.4 주요 C++ 타입

| 타입 | 뜻 |
| --- | --- |
| `LLVMContext` | LLVM 내부 자료구조의 소유자. 타입·상수 등이 여기 저장됨 |
| `Module` | 함수와 전역 변수의 묶음. 하나의 "번역 단위" |
| `Function` | 함수 하나 |
| `BasicBlock` | 기본 블록 하나 |
| `Value` | **계산 결과를 나타내는 모든 것의 부모 타입** |
| `IRBuilder<>` | 명령어를 만들어 넣어주는 도우미 |

`Value*`가 특히 중요합니다. 상수도, 명령어 결과도, 함수 인자도 전부 `Value*`
입니다. CodeGen의 함수들이 `Value*`를 주고받는 이유입니다.

### 1.5 `IRBuilder`

명령어를 직접 만들지 않고 빌더에게 시킵니다.

```cpp
Builder->SetInsertPoint(BB);                    // "여기에 넣어라"
Value *V = Builder->CreateFAdd(L, R, "addtmp"); // fadd 명령어 생성 + 삽입
```

빌더는 **현재 삽입 위치**를 기억합니다. 그래서 `CreateXxx`를 부르면 그 위치에
쌓입니다. 제어 흐름을 만들 때 `SetInsertPoint`로 위치를 옮겨 가며 씁니다.

`"addtmp"`는 사람이 읽기 위한 이름 힌트입니다. 중복되면 LLVM이 `addtmp1`,
`addtmp2`로 자동 조정합니다.

---

## 2. CodeGen 클래스

```cpp
class CodeGen : public ASTVisitor {
  OperatorTable &Ops;              // 우선순위표 (사용자 정의 연산자 등록용)
  DebugInfo *Dbg = nullptr;        // -g일 때만 존재

  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  std::unique_ptr<llvm::IRBuilder<>> Builder;

  bool OptimizeFunctions = true;
  std::unique_ptr<llvm::FunctionPassManager> FPM;
  std::unique_ptr<llvm::LoopAnalysisManager> LAM;
  std::unique_ptr<llvm::FunctionAnalysisManager> FAM;
  std::unique_ptr<llvm::CGSCCAnalysisManager> CGAM;
  std::unique_ptr<llvm::ModuleAnalysisManager> MAM;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> PIC;
  std::unique_ptr<llvm::StandardInstrumentations> SI;

  std::map<std::string, llvm::AllocaInst *> NamedValues;
  std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

  llvm::Value *Result = nullptr;
  ...
};
```

원본 튜토리얼에서 이 12개가 전부 **전역 변수**였습니다. 여기서는 멤버입니다.

`: public ASTVisitor` — [02-ast](02-ast.md)의 방문자 인터페이스를 구현합니다.
그래서 `visit()` 9개를 반드시 정의해야 합니다.

`Dbg`만 포인터인 이유: 디버그 정보는 `-g`일 때만 있으므로 "없음"을 표현해야
합니다. `Ops`는 항상 있으므로 참조입니다.

---

## 3. `initModule` — 모듈과 최적화 파이프라인 준비

```cpp
void CodeGen::initModule(StringRef ModuleName, const DataLayout &DL,
                         bool Optimize) {
  OptimizeFunctions = Optimize;

  Ctx = std::make_unique<LLVMContext>();
  Mod = std::make_unique<Module>(ModuleName, *Ctx);
  Mod->setDataLayout(DL);
  Builder = std::make_unique<IRBuilder<>>(*Ctx);

  // 모듈이 새로 생겼으니 분석 정보도 새로 만든다
  FPM  = std::make_unique<FunctionPassManager>();
  LAM  = std::make_unique<LoopAnalysisManager>();
  FAM  = std::make_unique<FunctionAnalysisManager>();
  CGAM = std::make_unique<CGSCCAnalysisManager>();
  MAM  = std::make_unique<ModuleAnalysisManager>();
  PIC  = std::make_unique<PassInstrumentationCallbacks>();
  SI   = std::make_unique<StandardInstrumentations>(*Ctx, false);
  SI->registerCallbacks(*PIC, MAM.get());

  FPM->addPass(PromotePass());        // alloca → SSA 레지스터 (mem2reg)
  FPM->addPass(InstCombinePass());    // 자잘한 패턴 정리
  FPM->addPass(ReassociatePass());    // 결합법칙으로 재배치
  FPM->addPass(GVNPass());            // 공통 부분식 제거
  FPM->addPass(SimplifyCFGPass());    // 제어 흐름 단순화

  PassBuilder PB;
  PB.registerModuleAnalyses(*MAM);
  PB.registerFunctionAnalyses(*FAM);
  PB.crossRegisterProxies(*LAM, *FAM, *CGAM, *MAM);
}
```

### 왜 "한 번만 만드는 객체"가 아닌가

이 함수는 **여러 번 불립니다.**

| 모드 | 호출 시점 |
| --- | --- |
| JIT | 함수 정의마다 1회 + 최상위 식마다 1회 |
| `-c` 컴파일 | 시작할 때 1회만 |

JIT은 완성된 모듈을 JIT에 넘겨버리므로(소유권 이전), 계속하려면 새 모듈을
열어야 합니다. 그래서 `CodeGen`은 **재설정 가능한** 객체로 설계돼 있습니다.
원본 튜토리얼의 `InitializeModuleAndManagers()`와 같은 역할입니다.

### 패스(pass)란

IR을 입력받아 더 나은 IR로 바꾸는 변환기입니다. 위 5개는 함수 단위로 돕니다.
`PromotePass`(mem2reg)가 특히 중요한데, 5절에서 다룹니다.

`FunctionAnalysisManager` 등은 패스들이 공유하는 **분석 결과 캐시**입니다.
지배 트리(dominator tree) 같은 것을 매번 다시 계산하지 않게 해 줍니다.

---

## 4. 식(expression) 코드 생성

### 4.1 진입점

```cpp
Value *CodeGen::codegenExpr(ExprAST &E) {
  Result = nullptr;
  E.accept(*this);
  return Result;
}
```

[02-ast](02-ast.md) 4.4절의 `Result` 트릭입니다. `accept`가 알맞은 `visit`을
부르고, `visit`이 `Result`를 채웁니다.

### 4.2 숫자

```cpp
void CodeGen::visit(NumberExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);
  Result = ConstantFP::get(*Ctx, APFloat(E.getVal()));
}
```

`ConstantFP`는 부동소수점 상수입니다. **명령어가 아니라 값**이라 어떤 블록에도
들어가지 않습니다. 같은 값을 여러 번 요청하면 LLVM이 같은 객체를 돌려줍니다.

`if (Dbg)` 패턴이 모든 `visit`에 나옵니다. `-g`가 아니면 `Dbg`가 `nullptr`이라
건너뜁니다.

### 4.3 변수 참조

```cpp
void CodeGen::visit(VariableExprAST &E) {
  AllocaInst *V = NamedValues[E.getName()];
  if (!V) {
    Result = logErrorV("Unknown variable name");
    return;
  }
  if (Dbg)
    Dbg->emitLocation(&E);
  Result = Builder->CreateLoad(Type::getDoubleTy(*Ctx), V, E.getName().c_str());
}
```

`NamedValues`는 **이름 → 스택 슬롯** 사전입니다. 변수를 읽는다는 것은 그
슬롯에서 값을 **불러오는(load)** 것입니다.

`std::map`의 `[]`는 키가 없으면 기본값(`nullptr`)을 만들어 넣습니다.
그래서 `if (!V)` 로 "선언되지 않은 변수"를 잡아냅니다.

### 4.4 이항 연산자

```cpp
void CodeGen::visit(BinaryExprAST &E) {
  if (Dbg) Dbg->emitLocation(&E);

  Value *L = codegenExpr(E.getLHS());
  Value *R = codegenExpr(E.getRHS());
  if (!L || !R) { Result = nullptr; return; }

  switch (E.getOp()) {
  case '+': Result = Builder->CreateFAdd(L, R, "addtmp"); return;
  case '-': Result = Builder->CreateFSub(L, R, "subtmp"); return;
  case '*': Result = Builder->CreateFMul(L, R, "multmp"); return;
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    Result = Builder->CreateUIToFP(L, Type::getDoubleTy(*Ctx), "booltmp");
    return;
  default:
    break;
  }

  // 내장 연산자가 아니면 사용자 정의 연산자 = 그냥 함수 호출
  Function *F = getFunction(std::string("binary") + E.getOp());
  if (!F) { Result = logErrorV("binary operator not found"); return; }

  Value *Ops2[] = {L, R};
  Result = Builder->CreateCall(F, Ops2, "binop");
}
```

`L`과 `R`을 **즉시 지역 변수에 받는 것**을 주목하세요. `Result` 규칙입니다.

`<` 만 두 단계인 이유: `fcmp`는 `i1`(1비트 불리언)을 돌려주는데 Kaleidoscope의
값은 전부 `double`이어야 하므로, `CreateUIToFP`로 `0.0`/`1.0`으로 바꿉니다.

`default: break;` 다음이 [사용자 정의 연산자](03-parser.md#7-parseprototype--함수-시그니처와-연산자-정의)
처리입니다. 특별한 것이 없습니다. **이름을 만들어 평범하게 호출**할 뿐입니다.

단항도 같습니다.

```cpp
Function *F = getFunction(std::string("unary") + E.getOpcode());
Result = Builder->CreateCall(F, OperandV, "unop");
```

### 4.5 대입

```cpp
void CodeGen::visit(AssignExprAST &E) {
  if (Dbg) Dbg->emitLocation(&E);

  Value *Val = codegenExpr(E.getValue());
  if (!Val) { Result = nullptr; return; }

  AllocaInst *Variable = NamedValues[E.getName()];
  if (!Variable) { Result = logErrorV("Unknown variable name"); return; }

  Builder->CreateStore(Val, Variable);
  Result = Val;
}
```

**캐스팅도 실패 경로도 없습니다.** 파서가 이미 "왼쪽은 식별자"를 보장했고
이름을 문자열로 넘겨줬기 때문입니다 ([03-parser](03-parser.md) 6절).

`Result = Val` — 대입식도 값을 가집니다. 그래서 `a = b = 5` 같은 연쇄가
원리상 가능합니다.

### 4.6 함수 호출

```cpp
void CodeGen::visit(CallExprAST &E) {
  Function *CalleeF = getFunction(E.getCallee());
  if (!CalleeF) { Result = logErrorV("Unknown function referenced"); return; }

  if (CalleeF->arg_size() != E.getArgs().size()) {
    Result = logErrorV("Incorrect # arguments passed");
    return;
  }

  std::vector<Value *> ArgsV;
  for (const auto &Arg : E.getArgs()) {
    ArgsV.push_back(codegenExpr(*Arg));
    if (!ArgsV.back()) { Result = nullptr; return; }
  }
  Result = Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}
```

인자 개수는 여기서 검사합니다. 타입은 전부 `double`이라 검사할 것이 없습니다.

### 4.7 `if` — PHI 노드가 나오는 곳

Kaleidoscope의 `if`는 **값을 내는 식**이므로 SSA에서 `phi`가 필요합니다.

```cpp
void CodeGen::visit(IfExprAST &E) {
  Value *CondV = codegenExpr(E.getCond());
  if (!CondV) { Result = nullptr; return; }

  // double 조건을 i1로: "0이 아니면 참"
  CondV = Builder->CreateFCmpONE(CondV, ConstantFP::get(*Ctx, APFloat(0.0)),
                                 "ifcond");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  BasicBlock *ThenBB  = BasicBlock::Create(*Ctx, "then", TheFunction);
  BasicBlock *ElseBB  = BasicBlock::Create(*Ctx, "else");     // 아직 미삽입
  BasicBlock *MergeBB = BasicBlock::Create(*Ctx, "ifcont");   // 아직 미삽입

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // then 블록
  Builder->SetInsertPoint(ThenBB);
  Value *ThenV = codegenExpr(E.getThen());
  if (!ThenV) { Result = nullptr; return; }
  Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();     // ★ 갱신

  // else 블록
  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = codegenExpr(E.getElse());
  if (!ElseV) { Result = nullptr; return; }
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();     // ★ 갱신

  // 합류 블록
  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*Ctx), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  Result = PN;
}
```

**★ 표시가 초보자가 가장 많이 틀리는 부분입니다.**

`E.getThen()`이 그 안에 또 `if`를 갖고 있으면, 그 코드 생성 도중 블록이 여러 개
추가됩니다. 그러면 `MergeBB`로 실제로 분기해 들어오는 블록은 처음 만든 `ThenBB`가
아니라 **마지막 블록**입니다. `phi`에는 정확한 선행 블록을 적어야 하므로
`Builder->GetInsertBlock()`으로 현재 위치를 다시 물어봅니다.

`ElseBB`와 `MergeBB`를 만들 때 함수를 넘기지 않은 것도 의도적입니다.
나중에 `TheFunction->insert(...)`로 **순서를 맞춰** 넣습니다.

### 4.8 `for` — do-while 이라는 함정

```cpp
void CodeGen::visit(ForExprAST &E) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, E.getVarName());

  Value *StartVal = codegenExpr(E.getStart());
  Builder->CreateStore(StartVal, Alloca);

  BasicBlock *LoopBB = BasicBlock::Create(*Ctx, "loop", TheFunction);
  Builder->CreateBr(LoopBB);          // ← 조건 검사 없이 무조건 진입
  Builder->SetInsertPoint(LoopBB);

  // 변수 가리기(shadowing) 저장
  AllocaInst *OldVal = NamedValues[E.getVarName()];
  NamedValues[E.getVarName()] = Alloca;

  if (!codegenExpr(E.getBody())) { Result = nullptr; return; }

  Value *StepVal = E.getStep() ? codegenExpr(*E.getStep())
                               : ConstantFP::get(*Ctx, APFloat(1.0));

  Value *EndCond = codegenExpr(E.getEnd());     // ← 조건을 여기서 계산

  Value *CurVar  = Builder->CreateLoad(Type::getDoubleTy(*Ctx), Alloca, ...);
  Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
  Builder->CreateStore(NextVar, Alloca);        // ← 그 다음에 증가

  EndCond = Builder->CreateFCmpONE(EndCond, ConstantFP::get(*Ctx, APFloat(0.0)),
                                   "loopcond");

  BasicBlock *AfterBB = BasicBlock::Create(*Ctx, "afterloop", TheFunction);
  Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  Builder->SetInsertPoint(AfterBB);

  if (OldVal) NamedValues[E.getVarName()] = OldVal;
  else        NamedValues.erase(E.getVarName());

  Result = Constant::getNullValue(Type::getDoubleTy(*Ctx));   // for는 항상 0.0
}
```

**두 가지 함정이 있습니다.**

1. **do-while이다.** 진입할 때 조건을 검사하지 않습니다. 그래서
   `for i = 1, i < 1 in ...` 도 본문이 **한 번은 실행됩니다.**
2. **조건을 증가 전에 계산한다.** `EndCond`가 `NextVar`보다 먼저입니다.

그 결과 C의 `for`보다 **한 번 더** 돕니다.

```
$ echo 'def binary : 1 (x y) y;
        def s(n) var t = 0 in (for i = 0, i < n, 2 in t = t + i) : t;
        s(10);' | ./build/toy
Evaluated to 30.000000        ← C 감각으로는 20 (0+2+4+6+8)
```

`i = 10`일 때도 본문이 돌아 `+10`이 더해집니다. 버그가 아니라 튜토리얼의 설계이며,
`tests/operators.ks`가 이 값을 고정해 두고 있습니다.

### 4.9 `var` — 지역 변수

```cpp
void CodeGen::visit(VarExprAST &E) {
  std::vector<AllocaInst *> OldBindings;
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  for (const auto &NamedVar : E.getVarNames()) {
    const std::string &VarName = NamedVar.first;
    ExprAST *Init = NamedVar.second.get();

    // ★ 변수를 scope에 넣기 "전에" 초기식을 계산
    Value *InitVal = Init ? codegenExpr(*Init)
                          : ConstantFP::get(*Ctx, APFloat(0.0));
    if (!InitVal) { Result = nullptr; return; }

    AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, VarName);
    Builder->CreateStore(InitVal, Alloca);

    OldBindings.push_back(NamedValues[VarName]);   // 가려진 것 저장
    NamedValues[VarName] = Alloca;
  }

  Value *BodyVal = codegenExpr(E.getBody());
  if (!BodyVal) { Result = nullptr; return; }

  unsigned Idx = 0;
  for (const auto &NamedVar : E.getVarNames())
    NamedValues[NamedVar.first] = OldBindings[Idx++];   // 복원

  Result = BodyVal;
}
```

★ 순서 덕분에 이런 코드가 바깥 `a`를 참조합니다.

```
var a = 1 in
  var a = a in ...     # 안쪽 a의 초기값은 바깥 a
```

`OldBindings`로 저장·복원하는 것이 **스코프**를 구현하는 방법입니다.

---

## 5. `alloca`와 mem2reg — 변수 대입의 해법

SSA에서는 이름에 두 번 대입할 수 없습니다. 그럼 `x = 5`는 어떻게 표현할까요?

**답: 변수를 메모리(스택)에 두고, 읽고 쓸 때마다 load/store를 쓴다.**
메모리는 SSA 규칙의 적용 대상이 아니기 때문입니다.

```cpp
AllocaInst *CodeGen::createEntryBlockAlloca(Function *TheFunction,
                                            StringRef VarName) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(*Ctx), nullptr, VarName);
}
```

`alloca`는 스택 공간을 잡는 명령어입니다. 여기서 **임시 빌더를 따로 만들어**
현재 위치가 아니라 **함수의 entry 블록 맨 앞**에 넣습니다.

**이것은 취향이 아니라 요구사항입니다.** `PromotePass`(mem2reg)는 entry 블록에
있는 `alloca`만 레지스터로 승격시킵니다. 다른 곳에 넣으면 최적화가 조용히
동작하지 않고, load/store가 그대로 남습니다.

효과를 직접 볼 수 있습니다.

```llvm
; 최적화 전 — 사람이 만들기 쉬운 형태
%x = alloca double
store double 5.0, ptr %x
%x1 = load double, ptr %x
%add = fadd double %x1, 1.0

; PromotePass 후 — SSA 레지스터로
%add = fadd double 5.0, 1.0
```

**이것이 Ch7의 핵심 교훈입니다.** 프론트엔드는 "쉬운 방법"(전부 스택)으로
IR을 만들고, PHI 노드 배치라는 어려운 문제는 LLVM에게 맡깁니다.

`if`에서는 왜 `phi`를 직접 만들었을까요? `if`의 결과값은 변수가 아니라
**식의 값**이라 스택에 넣을 대상이 아니기 때문입니다. 둘의 역할이 다릅니다.

---

## 6. 함수 코드 생성

### 6.1 프로토타입

```cpp
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
```

`std::vector<Type*> Doubles(n, double타입)` — "`double`을 n개 가진 벡터".
모든 인자 타입이 같으니 이렇게 한 줄로 만듭니다.

`FunctionType::get(반환타입, 인자타입들, 가변인자여부)` 로 함수 타입을 만들고,
`Function::Create`로 실제 함수를 모듈에 등록합니다.

### 6.2 `getFunction`과 `FunctionProtos`

```cpp
Function *CodeGen::getFunction(const std::string &Name) {
  if (auto *F = Mod->getFunction(Name))    // 현재 모듈에 있으면 그것
    return F;

  auto FI = FunctionProtos.find(Name);     // 없으면 프로토타입에서 선언 생성
  if (FI != FunctionProtos.end())
    return codegen(*FI->second);

  return nullptr;
}
```

JIT 모드는 **모듈을 계속 새로 만듭니다.** 이전 모듈에 있던 함수는 새 모듈에서
보이지 않습니다. 그래서 프로토타입을 따로 모아 두고, 필요할 때 새 모듈에
**선언만** 다시 만듭니다. 정의는 이미 JIT 안에 있으므로 링크가 됩니다.

### 6.3 함수 정의 — 가장 복잡한 함수

```cpp
Function *CodeGen::codegen(FunctionAST &F) {
  PrototypeAST &P = F.getProto();          // ★ 소유권 넘기기 "전에" 참조 확보
  FunctionProtos[P.getName()] = F.takeProto();

  Function *TheFunction = getFunction(P.getName());
  if (!TheFunction)
    return nullptr;

  // 연산자면 우선순위 등록 → 이제부터 파서가 쓸 수 있음
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
    Dbg->emitLocation(nullptr);            // 프롤로그는 위치 없음
  }

  // 인자를 스택 슬롯으로
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
    if (Dbg) Dbg->popScope();

    verifyFunction(*TheFunction);          // IR이 올바른지 검사

    if (OptimizeFunctions && FPM)
      FPM->run(*TheFunction, *FAM);        // 최적화 패스 실행

    return TheFunction;
  }

  // 본문 생성 실패 → 함수 제거
  TheFunction->eraseFromParent();
  if (P.isBinaryOp())
    Ops.erase(P.getOperatorName());        // 등록했던 우선순위도 취소
  if (Dbg) Dbg->popScope();
  return nullptr;
}
```

읽을 포인트 넷:

**(1) `NamedValues.clear()`** — 함수마다 스코프를 새로 시작합니다.
Kaleidoscope에는 전역 변수가 없어서 이렇게 단순합니다.

**(2) 인자도 스택에 복사한다** — 인자는 `Value*`로 이미 있지만, `alloca`를
만들어 저장합니다. 그래야 인자에 대입할 수 있고, mem2reg가 다시 레지스터로
되돌려 주므로 손해가 없습니다.

**(3) 우선순위 등록 시점** — 코드 생성 시점이라, 연산자는 **정의되고 코드가
생성된 뒤에야** 쓸 수 있습니다. 실패하면 `Ops.erase`로 되돌립니다.

**(4) ★ 원본 튜토리얼의 버그를 고친 부분**

```cpp
PrototypeAST &P = F.getProto();          // 먼저 참조를 잡고
FunctionProtos[P.getName()] = F.takeProto();   // 그 다음 소유권 이전
```

`takeProto()` 후 `F`의 `unique_ptr`은 비어 있습니다. 객체 자체는 살아 있으므로
`P`는 유효합니다. 원본 튜토리얼은 실패 경로에서 이렇게 씁니다.

```cpp
// 원본 튜토리얼 (Ch9 1310행)
if (P.isBinaryOp())
    BinopPrecedence.erase(Proto->getOperatorName());   // Proto는 이미 비었음!
```

`Proto`가 `nullptr`인데 `->`로 접근합니다. 본문 생성이 실패할 때만 터지는
잠복 버그입니다. 이 저장소는 `P.getOperatorName()`을 씁니다.

---

## 7. 직접 확인해 보기

```bash
# IR 보기 (함수 정의를 입력하면 IR이 출력됨)
echo 'def add2(x) x + 2;' | ./build/toy

# mem2reg 효과: alloca/load/store가 사라진 것을 확인
echo 'def f(x) var a = x in (a = a + 1);' | ./build/toy

# if의 PHI 노드
echo 'def f(x) if x < 0 then 0 else x;' | ./build/toy

# 사용자 정의 연산자가 그냥 call인 것
echo 'def unary!(v) if v then 0 else 1;
      def binary | 5 (a b) if a then 1 else !!b;' | ./build/toy
```

---

## 8. 정리

- LLVM IR은 SSA다. 이름에 두 번 대입할 수 없다
- `IRBuilder`는 삽입 위치를 기억한다. 제어 흐름은 `SetInsertPoint`로 옮겨 가며 만든다
- `if`는 값을 내므로 `phi`가 필요하다. **선행 블록은 `GetInsertBlock()`으로 다시 확인**할 것
- `for`는 do-while이고 조건이 증가보다 먼저다 → C보다 한 번 더 돈다
- 변수 대입은 `alloca` + load/store로 하고, mem2reg가 SSA로 되돌린다.
  **`alloca`는 반드시 entry 블록에**
- 사용자 정의 연산자는 이름이 `binary|`인 평범한 함수 호출이다
- `CodeGen`은 재설정 가능한 객체다 (JIT이 모듈을 계속 가져가므로)
- `takeProto()` 전에 `getProto()` 참조를 확보할 것 (원본 버그)

**다음**: [05. DebugInfo](05-debuginfo.md) — DWARF 디버그 정보
