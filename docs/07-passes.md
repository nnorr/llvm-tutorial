# 07. 최적화 패스와 Pass Manager

**파일**: `src/CodeGen.cpp` (`initModule`, `codegen(FunctionAST&)`), `src/ObjectEmitter.cpp`
**원본 튜토리얼**: Chapter 4 — *Adding JIT and Optimizer Support*

---

## 1. 패스란

**패스(pass)** 는 IR을 받아서 IR을 내놓는 변환기입니다.

```
IR ──▶ [ mem2reg ] ──▶ IR' ──▶ [ instcombine ] ──▶ IR'' ──▶ …
```

두 종류가 있습니다.

| 종류 | 하는 일 | 예 |
| --- | --- | --- |
| **변환(transform) 패스** | IR을 고친다 | `PromotePass`, `GVNPass` |
| **분석(analysis) 패스** | IR을 조사해 정보를 만든다 (고치지 않음) | 지배 트리, 별칭 분석 |

변환 패스는 대개 분석 결과를 필요로 합니다. 예를 들어 mem2reg는 **지배 트리
(dominator tree)** 가 있어야 어느 `load`가 어느 `store`를 보는지 알 수 있습니다.

이 관계를 관리하는 것이 **Pass Manager**와 **Analysis Manager**입니다.

패스가 도는 **범위**도 나뉩니다.

| 범위 | 매니저 |
| --- | --- |
| 함수 하나 | `FunctionPassManager` |
| 모듈 전체 | `ModulePassManager` |
| 루프 하나 | `LoopPassManager` |
| 호출 그래프 묶음 | `CGSCCPassManager` |

이 프로젝트는 **함수 패스만** 씁니다. 함수를 하나 만들 때마다 그 함수에만
돌리기 때문입니다.

---

## 2. 새 Pass Manager와 구식 Pass Manager

LLVM에는 패스 매니저가 **두 개** 있습니다. 역사적 이유입니다.

| | Legacy PM | New PM |
| --- | --- | --- |
| 도입 | 초기부터 | LLVM 6 이후 점진 전환 |
| 분석 관리 | 패스가 직접 요구 | 별도 `AnalysisManager` |
| 이 프로젝트 | **백엔드 코드 생성** | **최적화** |

**이 저장소는 둘 다 씁니다. 오타가 아닙니다.**

```cpp
// src/CodeGen.cpp — 최적화는 새 PM
FPM = std::make_unique<FunctionPassManager>();
FPM->addPass(PromotePass());

// src/ObjectEmitter.cpp — 코드 생성은 구식 PM
legacy::PassManager Pass;
TM.addPassesToEmitFile(Pass, Dest, nullptr, CodeGenFileType::ObjectFile);
```

이유: **LLVM의 백엔드(IR → 기계어) 파이프라인이 아직 새 PM으로 이전되지
않았습니다.** `addPassesToEmitFile`은 `legacy::PassManager`만 받습니다.
현재 LLVM의 실제 상태이며, 우리가 선택한 것이 아닙니다.

`legacy::` 라는 네임스페이스 이름 자체가 "구식"임을 드러냅니다.

---

## 3. `initModule`이 파이프라인을 세우는 과정

```cpp
void CodeGen::initModule(StringRef ModuleName, const DataLayout &DL,
                         bool Optimize) {
  OptimizeFunctions = Optimize;

  Ctx = std::make_unique<LLVMContext>();
  Mod = std::make_unique<Module>(ModuleName, *Ctx);
  Mod->setDataLayout(DL);
  Builder = std::make_unique<IRBuilder<>>(*Ctx);

  // ── (1) 매니저 생성 ────────────────────────────────
  FPM  = std::make_unique<FunctionPassManager>();
  LAM  = std::make_unique<LoopAnalysisManager>();
  FAM  = std::make_unique<FunctionAnalysisManager>();
  CGAM = std::make_unique<CGSCCAnalysisManager>();
  MAM  = std::make_unique<ModuleAnalysisManager>();
  PIC  = std::make_unique<PassInstrumentationCallbacks>();
  SI   = std::make_unique<StandardInstrumentations>(*Ctx, /*DebugLogging*/ false);
  SI->registerCallbacks(*PIC, MAM.get());

  // ── (2) 변환 패스 등록 ─────────────────────────────
  FPM->addPass(PromotePass());
  FPM->addPass(InstCombinePass());
  FPM->addPass(ReassociatePass());
  FPM->addPass(GVNPass());
  FPM->addPass(SimplifyCFGPass());

  // ── (3) 분석 패스 등록 ─────────────────────────────
  PassBuilder PB;
  PB.registerModuleAnalyses(*MAM);
  PB.registerFunctionAnalyses(*FAM);
  PB.crossRegisterProxies(*LAM, *FAM, *CGAM, *MAM);
}
```

### (1) 매니저 7개

| 객체 | 역할 |
| --- | --- |
| `FPM` | **변환 패스 목록**. 실제로 실행할 것들 |
| `FAM` | 함수 단위 분석 결과 캐시 ← 실제로 쓰이는 것 |
| `LAM`, `CGAM`, `MAM` | 루프/호출그래프/모듈 단위 캐시 |
| `PIC` | 패스 실행 전후에 끼어들 콜백 등록소 |
| `SI` | 표준 계측(디버그 로깅, 검증 등) |

**우리는 함수 패스만 쓰는데 왜 `LAM`, `CGAM`, `MAM`까지 만드나?**
(3)의 `crossRegisterProxies`가 네 개를 전부 요구하기 때문입니다. 서로 다른
범위의 매니저가 상대의 정보를 찾아갈 수 있도록 연결 통로(proxy)를 놓는데,
그러려면 네 개가 다 있어야 합니다. 실제로 쓰이는 것은 `FAM` 하나입니다.

### (2) 변환 패스 5개

등록 **순서대로** 실행됩니다. 순서가 중요합니다 — 4절에서 봅니다.

### (3) 분석 등록

```cpp
PassBuilder PB;
PB.registerModuleAnalyses(*MAM);
PB.registerFunctionAnalyses(*FAM);
PB.crossRegisterProxies(*LAM, *FAM, *CGAM, *MAM);
```

`PassBuilder`는 "표준 분석 패스들을 한 번에 등록해 주는 도우미"입니다.
`registerFunctionAnalyses(*FAM)`가 지배 트리, 별칭 분석 등을 `FAM`에 채웁니다.
이걸 빠뜨리면 `PromotePass`가 지배 트리를 요청할 때 없어서 죽습니다.

`crossRegisterProxies`는 범위 간 연결입니다. 함수 패스가 모듈 수준 분석을
필요로 할 때 이 통로로 찾아갑니다.

### 왜 모듈마다 다시 만드나

분석 결과는 **특정 모듈의 IR에 대한** 정보입니다. 모듈이 바뀌면 전부 무효입니다.
JIT 모드는 함수 정의마다, 최상위 식마다 모듈을 새로 여므로
([06-backend-and-driver](06-backend-and-driver.md) 3.5절) 매니저도 함께
새로 만듭니다.

---

## 4. 패스 5개, 실제 동작

`opt` 도구로 하나씩 직접 확인할 수 있습니다. 아래 출력은 전부 실제 실행 결과입니다.

### 4.1 `PromotePass` (mem2reg) — 가장 중요

**스택 슬롯을 SSA 레지스터로 승격**시킵니다.

[04-codegen](04-codegen.md) 5절에서 본 대로, 프론트엔드는 변수를 전부
`alloca` + `load`/`store`로 만듭니다. 그게 훨씬 쉽기 때문입니다.

```bash
$ cat pre.ll
define double @f(double %x) {
entry:
  %x.addr = alloca double
  store double %x, ptr %x.addr
  %a = alloca double
  %x1 = load double, ptr %x.addr
  store double %x1, ptr %a
  %a1 = load double, ptr %a
  %addtmp = fadd double %a1, 1.000000e+00
  store double %addtmp, ptr %a
  %a2 = load double, ptr %a
  ret double %a2
}

$ opt -passes=mem2reg -S pre.ll
define double @f(double %x) {
entry:
  %addtmp = fadd double %x, 1.000000e+00
  ret double %addtmp
}
```

**10줄이 2줄로.** `alloca`, `load`, `store`가 전부 사라지고 PHI 노드가 필요한
곳에는 자동으로 삽입됩니다.

이 패스가 있기 때문에 [Ch7의 변경 가능한 변수](04-codegen.md#5-alloca와-mem2reg--변수-대입의-해법)를
프론트엔드에서 손쉽게 구현할 수 있습니다. 그래서 **목록의 첫 번째**입니다.
뒤 패스들도 SSA 형태를 전제로 하므로 순서상 먼저여야 합니다.

> **다시 강조**: mem2reg는 **entry 블록에 있는 `alloca`만** 승격시킵니다.
> `createEntryBlockAlloca`가 임시 빌더로 entry 맨 앞에 넣는 이유입니다.
> 다른 곳에 넣으면 이 패스가 조용히 아무것도 안 합니다.

### 4.2 `InstCombinePass`

지역적인 패턴을 더 나은 형태로 바꿉니다. "peephole 최적화"라고 부릅니다.

```
x + 0        →  x
x * 2        →  x + x   (또는 시프트)
(x + 1) - 1  →  x
```

Kaleidoscope처럼 `double`만 쓰는 코드에서는 정수 비트 연산 규칙 대부분이
해당되지 않아 효과가 제한적입니다.

### 4.3 `ReassociatePass` — 실제로는 거의 동작하지 않음

결합법칙으로 식을 재배치해 뒤 패스가 공통 부분식을 찾기 쉽게 만듭니다.

```
(a + b) + c  →  a + (b + c)   같은 재배치
```

**그런데 부동소수점에서는 결합법칙이 성립하지 않습니다.**

```
(1e30 + (-1e30)) + 1  =  1
1e30 + ((-1e30) + 1)  =  0     ← 값이 다름
```

그래서 LLVM은 `fadd`에 **fast-math 플래그**(`reassoc`)가 붙어 있지 않으면
재배치하지 않습니다. 그리고 Kaleidoscope의 CodeGen은 이 플래그를 붙이지
않습니다.

```bash
$ echo 'def f(a b) (a+b) * (a+b);' | ./build/toy
  %addtmp = fadd double %a, %b        ← reassoc 플래그 없음
  %multmp = fmul double %addtmp, %addtmp
```

즉 **이 패스는 우리 코드에서 사실상 아무 일도 하지 않습니다.** 튜토리얼이
"이런 것도 있다"는 예시로 넣은 것이며, 정수 코드를 다루는 언어였다면 의미가
있었을 것입니다.

### 4.4 `GVNPass` — 이건 실제로 동작

**G**lobal **V**alue **N**umbering. 같은 값을 두 번 계산하면 하나로 합칩니다.

공통 부분식 제거는 **결합법칙과 무관**하므로 fast-math 없이도 안전합니다.
위 예제가 그 증거입니다.

```llvm
; 우리 컴파일러가 def f(a b) (a+b) * (a+b); 에 대해 실제로 내는 결과
%addtmp = fadd double %a, %b
%multmp = fmul double %addtmp, %addtmp    ← (a+b)를 한 번만 계산
```

`fadd`가 두 번 나오지 않습니다. GVN이 두 번째를 지웠습니다.

### 4.5 `SimplifyCFGPass`

제어 흐름을 단순화합니다. 도달 불가능한 블록 제거, 블록 병합, 그리고
**분기를 `select`로 바꾸기**.

```bash
$ cat cfg.ll
define double @h(double %v) {
entry:
  %ifcond = fcmp one double %v, 0.000000e+00
  br i1 %ifcond, label %then, label %else
then:
  br label %ifcont
else:
  br label %ifcont
ifcont:
  %iftmp = phi double [ 0.000000e+00, %then ], [ 1.000000e+00, %else ]
  ret double %iftmp
}

$ opt -passes=simplifycfg -S cfg.ll
define double @h(double %v) {
entry:
  %ifcond = fcmp one double %v, 0.000000e+00
  %. = select i1 %ifcond, double 0.000000e+00, double 1.000000e+00
  ret double %.
}
```

블록 4개가 1개로, `phi`가 `select`로 바뀌었습니다. `select`는 분기 없이
값을 고르는 명령어라 CPU 분기 예측 실패가 없습니다.

**우리 컴파일러의 실제 출력에서도 볼 수 있습니다.**

```bash
$ echo 'def unary!(v) if v then 0 else 1;' | ./build/toy
define double @"unary!"(double %v) {
entry:
  %ifcond = fcmp ueq double %v, 0.000000e+00
  %. = select i1 %ifcond, double 1.000000e+00, double 0.000000e+00
  ret double %.
}
```

[04-codegen](04-codegen.md) 4.7절에서 애써 만든 `then`/`else`/`ifcont` 블록과
PHI 노드가 전부 사라졌습니다. **프론트엔드는 단순하고 정직하게 만들고,
정리는 패스에게 맡긴다** — 이것이 LLVM을 쓰는 방식입니다.

### 요약

| 패스 | Kaleidoscope에서 효과 |
| --- | --- |
| `PromotePass` | **매우 큼** — 변수 구현 방식 자체를 떠받침 |
| `InstCombinePass` | 작음 (double만 쓰므로) |
| `ReassociatePass` | **거의 없음** (fast-math 플래그 없음) |
| `GVNPass` | 있음 — 중복 계산 제거 |
| `SimplifyCFGPass` | 큼 — if를 select로 |

---

## 5. 언제 실행되나

`CodeGen::codegen(FunctionAST&)` 끝부분입니다.

```cpp
if (Value *RetVal = visit(F.getBody())) {
  Builder->CreateRet(RetVal);
  ...
  verifyFunction(*TheFunction);           // 먼저 IR이 올바른지 검사

  if (OptimizeFunctions && FPM)
    FPM->run(*TheFunction, *FAM);         // 그 다음 최적화

  return TheFunction;
}
```

**함수 하나를 완성할 때마다 그 함수에만** 돌립니다. 모듈 전체를 나중에 한 번에
돌리는 것이 아닙니다. REPL에서 함수를 정의하자마자 최적화된 IR이 출력되는
이유입니다.

`FPM->run(함수, 분석매니저)` — 변환 패스가 필요한 분석을 `FAM`에서 꺼내 씁니다.

`verifyFunction`이 **먼저**인 것도 의도적입니다. 잘못된 IR을 패스에 넣으면
패스가 이상하게 죽어서 원인 파악이 어렵습니다. 검증을 먼저 하면 프론트엔드
버그를 그 자리에서 잡습니다.

---

## 6. 최적화를 끄는 경우

```cpp
CG.initModule(O.Input, TM->createDataLayout(), /*Optimize=*/!O.Debug);
```

`-g`(디버그 정보)일 때 끕니다. 이유는
[05-debuginfo](05-debuginfo.md) 5절 — 최적화가 명령어를 재배치·삭제하면
줄 번호가 소스와 어긋나 디버깅이 어려워집니다. 원본 튜토리얼 Ch9도 패스를
전혀 실행하지 않습니다.

`OptimizeFunctions` 플래그는 `initModule`에 저장되고 위 `if`에서 확인됩니다.
`-c`만 준 경우는 해당하지 않습니다 — 최적화는 그대로 돕니다.

이 제약이 어디서 오는지, 진짜 컴파일러는 왜 `-O2 -g`를 할 수 있는지는
[10. 디버그 정보와 최적화](10-debuginfo-and-optimization.md)에서 다룹니다.

---

## 7. `StandardInstrumentations`는 무엇인가

```cpp
SI = std::make_unique<StandardInstrumentations>(*Ctx, /*DebugLogging*/ false);
SI->registerCallbacks(*PIC, MAM.get());
```

패스 실행 **전후에 끼어드는 훅**들의 표준 묶음입니다. 제공하는 기능:

- 패스 실행 로그 (`-debug-pass-manager` 같은 옵션)
- 패스가 IR을 망가뜨리지 않았는지 자동 검증
- 패스 전후 IR 출력

`DebugLogging`을 `false`로 둬서 조용히 동작합니다. (원본 튜토리얼은 `true`인데,
REPL 출력이 지저분해져서 껐습니다.)

당장 없어도 동작하지만, 패스를 디버깅할 때 켜면 유용합니다.

---

## 8. 직접 실험하기

`opt`는 conda 환경에 이미 들어 있습니다. 패스를 개별적으로 실험할 수 있습니다.

```bash
conda activate llvm-tut

# 우리 컴파일러의 IR을 파일로 (JIT 모드는 최적화 후 IR을 출력)
echo 'def f(x) if x < 0 then 0 else x;' | ./build/toy 2>&1 \
  | sed -n '/define/,/^}/p' > f.ll

# 패스 하나만 적용
opt -passes=mem2reg    -S f.ll
opt -passes=simplifycfg -S f.ll

# 여러 개 순서대로 (우리 파이프라인과 동일)
opt -passes='mem2reg,instcombine,reassociate,gvn,simplifycfg' -S f.ll

# 표준 최적화 레벨
opt -passes='default<O2>' -S f.ll

# 어떤 패스가 실제로 IR을 바꿨는지 보기
opt -passes='mem2reg,gvn' -S f.ll -print-changed 2>&1 | head -40

# 사용 가능한 패스 목록
opt --print-passes | head -40
```

`-print-changed`가 특히 유용합니다. 실제로 변화를 일으킨 패스만 골라 보여줍니다.

---

## 9. 정리

- 패스 = IR → IR 변환기. 변환 패스와 분석 패스가 있다
- LLVM에 패스 매니저가 둘 있고, **이 프로젝트는 둘 다 쓴다**.
  최적화는 새 PM, 백엔드 코드 생성은 구식 PM (LLVM의 현재 상태)
- 매니저 7개 중 실제로 쓰는 건 `FPM`과 `FAM`. 나머지는 `crossRegisterProxies`가
  요구해서 만든다
- `PassBuilder::registerFunctionAnalyses`를 빠뜨리면 패스가 분석을 못 찾아 죽는다
- 모듈이 새로 생기면 분석 결과가 무효 → 매니저도 새로 만든다
- **`PromotePass`(mem2reg)가 핵심.** 프론트엔드가 alloca로 쉽게 만들고
  SSA 변환은 여기에 맡긴다. entry 블록의 alloca만 승격된다
- `ReassociatePass`는 부동소수점 + fast-math 플래그 없음 조합이라 사실상 무동작
- `SimplifyCFGPass`가 `if`의 분기와 PHI를 `select`로 접는다
- 최적화는 **함수 하나 완성 시마다** 그 함수에만 실행된다. `verifyFunction`이 먼저
- `opt` 도구로 패스를 개별 실험할 수 있다

**다음**: [08. JIT](08-jit.md) — 컴파일한 코드를 메모리에서 실행하기
