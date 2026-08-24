# 06. Backend & Driver — 오브젝트 파일과 전체 구동

**파일**: `include/ObjectEmitter.h`, `src/ObjectEmitter.cpp`, `src/main.cpp`
**원본 튜토리얼**: Chapter 8 — *Compiling to Object Code*, Chapter 4 (JIT)

---

## 1. IR 다음에 갈 수 있는 두 갈래

`CodeGen`이 만든 `Module`은 두 소비자 중 하나에게 넘어갑니다.

```
                    ┌──▶ KaleidoscopeJIT  ──▶ 메모리에서 바로 실행   (기본)
Module ─────────────┤
                    └──▶ ObjectEmitter    ──▶ output.o 파일          (-c)
```

중요한 점: **`CodeGen`은 자기가 어느 쪽에 쓰이는지 모릅니다.** 필요한 것은
`DataLayout`(타입 크기·정렬 규칙) 하나뿐이고, 그건 JIT이든 TargetMachine이든
제공합니다. 덕분에 두 백엔드가 공존할 수 있습니다.

원본 튜토리얼은 파일 하나에 모든 것이 전역으로 있어 이 공존이 불가능했습니다.
그래서 Ch8은 JIT을 **삭제하고** 오브젝트 출력으로 바꿉니다.

---

## 2. ObjectEmitter

### 2.1 인터페이스

```cpp
class ObjectEmitter {
public:
  static void initializeTargets();

  static std::unique_ptr<llvm::TargetMachine>
  createHostTargetMachine(std::string &Error);

  static bool emit(llvm::Module &Mod, llvm::TargetMachine &TM,
                   const std::string &Filename, std::string &Error);
};
```

`static` 멤버 함수는 **객체 없이 호출**합니다. `ObjectEmitter::emit(...)`
처럼요. 상태를 가질 필요가 없어서 이렇게 했습니다.

에러를 `std::string &Error`로 돌려주는 이유: LLVM은 예외(exception)를 쓰지
않도록 빌드되는 것이 보통이라, 실패를 반환값과 출력 매개변수로 전달합니다.

### 2.2 타깃 초기화

```cpp
void ObjectEmitter::initializeTargets() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
}
```

LLVM은 x86, ARM, RISC-V … 여러 백엔드를 갖고 있고, 쓰기 전에 등록해야 합니다.

원본 튜토리얼은 `InitializeAllTargetInfos()` 등 **전부** 등록합니다. 그러면
모든 백엔드가 링크되어 실행 파일이 커집니다. 우리는 호스트용 오브젝트만
만들므로 `Native` 버전이면 충분합니다.

> 이건 실제로 겪은 문제입니다. `InitializeAll*`을 쓰면
> `LLVMInitializeAVRTargetInfo` 같은 심볼을 못 찾아 링크가 실패합니다.
> CMake에서 `native` 컴포넌트만 링크하기 때문입니다. 모든 백엔드를 링크하거나
> `Native`만 초기화하거나 둘 중 하나인데, 후자가 맞습니다.

### 2.3 TargetMachine 만들기

```cpp
std::unique_ptr<TargetMachine>
ObjectEmitter::createHostTargetMachine(std::string &Error) {
  auto TargetTriple = sys::getDefaultTargetTriple();

  const Target *TheTarget = TargetRegistry::lookupTarget(TargetTriple, Error);
  if (!TheTarget)
    return nullptr;

  const char *CPU = "generic";
  const char *Features = "";
  TargetOptions Opt;

  return std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
      TargetTriple, CPU, Features, Opt, Reloc::PIC_));
}
```

**타깃 트리플**은 `x86_64-unknown-linux-gnu` 같은 문자열로
"아키텍처-벤더-OS-ABI"를 나타냅니다. `getDefaultTargetTriple()`이 현재
머신의 것을 알려줍니다.

- `CPU = "generic"` — 특정 CPU 전용 명령어를 쓰지 않음
- `Reloc::PIC_` — 위치 독립 코드(Position Independent Code). 현대 리눅스에서
  공유 라이브러리·실행 파일의 기본입니다

### 2.4 오브젝트 파일 출력

```cpp
bool ObjectEmitter::emit(Module &Mod, TargetMachine &TM,
                         const std::string &Filename, std::string &Error) {
  Mod.setTargetTriple(TM.getTargetTriple().getTriple());
  Mod.setDataLayout(TM.createDataLayout());

  std::error_code EC;
  raw_fd_ostream Dest(Filename, EC, sys::fs::OF_None);
  if (EC) {
    Error = "Could not open file: " + EC.message();
    return false;
  }

  legacy::PassManager Pass;
  if (TM.addPassesToEmitFile(Pass, Dest, nullptr, CodeGenFileType::ObjectFile)) {
    Error = "TargetMachine can't emit a file of this type";
    return false;
  }

  Pass.run(Mod);
  Dest.flush();
  return true;
}
```

`addPassesToEmitFile`이 **IR → 기계어** 파이프라인 전체(명령어 선택, 레지스터
할당, 스케줄링 …)를 구성해 줍니다. 우리가 할 일은 실행뿐입니다.

> **`legacy::PassManager`?**
> [04-codegen](04-codegen.md)에서는 새 패스 매니저(`FunctionPassManager`)를
> 썼는데 여기서는 구식입니다. 오타가 아닙니다. LLVM의 **백엔드 코드 생성
> 파이프라인은 아직 새 패스 매니저로 이전되지 않았습니다.** 최적화는 새 것,
> 코드 생성은 구식 — 현재 LLVM의 실제 상태입니다.

`raw_fd_ostream`은 LLVM의 파일 출력 스트림입니다.

---

## 3. main.cpp — 드라이버

### 3.1 옵션

```cpp
struct Options {
  bool Compile = false;      // -c
  bool Debug = false;        // -g
  bool DumpAST = false;      // --dump-ast
  bool EmitLLVM = false;     // --emit-llvm
  std::string Input;
  std::string Output = "output.o";
};
```

파싱은 단순한 루프입니다. LLVM에 `cl::opt`라는 옵션 라이브러리가 있지만,
옵션이 5개뿐이라 직접 처리하는 편이 읽기 쉽습니다.

```cpp
if (O.Debug && !O.Compile) {
  errs() << "-g requires -c: debug info describes a source file, which the "
            "interactive JIT does not have\n";
  return 1;
}
```

[05-debuginfo](05-debuginfo.md) 2절의 제약을 여기서 강제합니다.

### 3.2 라이브러리 함수

```cpp
extern "C" DLLEXPORT double putchard(double X) {
  fputc(static_cast<char>(X), stderr);
  return 0;
}

extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}
```

Kaleidoscope에는 입출력이 없습니다. 대신 `extern`으로 이 C++ 함수들을
불러 쓸 수 있습니다.

```
extern putchard(c);
putchard(65);        # 'A' 출력
```

`extern "C"`는 **C++ 이름 변형(name mangling)을 끄라**는 뜻입니다. C++는
오버로딩 때문에 함수 이름에 타입 정보를 섞어 `_Z8putchardd` 같은 심볼을
만듭니다. 그러면 JIT이 `"putchard"`로 찾을 수 없습니다.

이와 짝이 되는 설정이 `CMakeLists.txt`에 있습니다.

```cmake
set_target_properties(toy PROPERTIES ENABLE_EXPORTS ON)
```

JIT은 심볼을 **실행 중인 프로세스 자신**에게서 찾습니다. 그런데 실행 파일의
심볼은 기본적으로 동적 심볼 테이블에 올라가지 않습니다. 이 설정이 `-rdynamic`을
붙여 노출시킵니다. 없으면 링크는 되는데 실행 중에 `putchard`를 못 찾습니다.

### 3.3 드라이버 루프 — 무엇이 한 번에 처리되는가

두 드라이버 모두 같은 골격입니다. **최상위 구문 하나를 파싱하고, 바로 코드를
생성하고, 다음으로 넘어갑니다.**

```
while (입력이 남아 있는 동안) {
    현재 토큰을 보고 셋 중 하나로 분기
      ├── tok_def     → parseDefinition()   → FunctionAST  → CG.codegen(*FnAST)
      ├── tok_extern  → parseExtern()       → PrototypeAST → CG.codegen(*ProtoAST)
      └── 그 외        → parseTopLevelExpr() → FunctionAST  → CG.codegen(*FnAST)
}
```

여기서 "구문"은 **문법 층의 단위**입니다. LLVM과는 아직 무관합니다.
`tests/fib.ks`는 `def` 3개 + 최상위 식 1개 = 구문 4개입니다.

**파일 전체를 아우르는 AST는 만들어지지 않습니다.** 파서가 다 읽고 나서 CodeGen이
도는 것이 아니라, 구문 하나마다 파싱과 코드 생성이 번갈아 일어납니다. 트리는
구문마다 새로 태어나고 죽습니다.

```cpp
if (auto FnAST = P.parseDefinition()) {   // ← 트리 하나가 여기서 생기고
    CG.codegen(*FnAST);
}                                          // ← 스코프를 벗어나며 통째로 해제
```

`FnAST`는 지역 `unique_ptr`이고, 그것이 트리 전체의 유일한 소유자입니다
([02-ast](02-ast.md) 7절). 예외가 하나 있습니다 — `codegen(FunctionAST&)`가
`takeProto()`로 프로토타입만 떼어내 `CodeGen::FunctionProtos`로 옮깁니다.
트리가 죽어도 나중 모듈에 함수를 다시 **선언**할 수 있어야 하기 때문입니다.

#### 코드 생성의 진입점은 `visit()`이 아니다

각 트리의 루트는 `FunctionAST`이고, `FunctionAST`와 `PrototypeAST`는
**`ExprAST`를 상속하지 않습니다.** `ExprASTKind`에 항목이 없으니 방문자의
스위치로 갈 수가 없고, 그래서 전용 오버로드로 받습니다.

```cpp
Function *CodeGen::codegen(PrototypeAST &P);   // 방문자 밖
Function *CodeGen::codegen(FunctionAST &F);    // 방문자 밖
```

방문자는 그 안에서 몸통에 대해 한 번 시작됩니다.

```cpp
// codegen(FunctionAST&) 내부
if (Value *RetVal = visit(F.getBody())) {      // ← 여기부터 식 트리 재귀
  Builder->CreateRet(RetVal);
```

즉 두 층입니다. **함수 골격(entry 블록, 인자 alloca, 디버그 스코프)은
`codegen(FunctionAST&)`가 직접 만들고, 몸통 식만 방문자가 훑습니다.**

최상위 식도 예외가 아닙니다. 파서가 인자 0개짜리 익명 함수로 감싸므로
([03-parser](03-parser.md) 8절), `1 + 2 * 3;` 도 루트가 `FunctionAST`입니다.

```
$ echo '1 + 2 * 3;' | ./build/toy --dump-ast
Function
  Prototype __anon_expr () @1      ← 파서가 씌운 껍데기
  Body:
    Binary '+' @1:3
```

덕분에 `def`든 최상위 식이든 코드 생성 경로가 하나로 통일됩니다.

#### 구문과 LLVM 모듈은 다른 층이다

**둘의 대응 관계가 모드마다 다릅니다.** 이것이 두 드라이버의 진짜 차이입니다.

| | 구문 4개짜리 `fib.ks` | `initModule` 호출 |
| --- | --- | --- |
| JIT 모드 | 모듈 **4개** | 구문마다 |
| `-c` 모드 | 모듈 **1개** | 시작할 때 한 번 |

JIT은 `def`와 최상위 식을 처리할 때마다 완성된 모듈을 JIT에 넘기고 새 모듈을
엽니다(3.5절). `-c`는 `main.cpp`의 루프 안에 `initModule`이 아예 없습니다 —
모든 함수가 한 모듈에 쌓였다가 마지막에 통째로 오브젝트 파일이 됩니다.

`extern`은 어느 쪽에서도 모듈 경계를 만들지 않습니다. 선언만 현재 모듈에 남고
`takeModule()`을 하지 않습니다.

**같은 `CodeGen`이 두 정책을 다 돌릴 수 있는 것은 `initModule`을 언제 부를지가
드라이버 쪽에 있기 때문입니다.** 원본 튜토리얼처럼 `TheModule`이 전역이었다면
두 경로가 하나의 모듈을 두고 다투게 되고, 실제로 원본은 Ch8에서 JIT을 삭제하는
방식으로 이 충돌을 피합니다.

#### 생성된 IR 보기 — EOF 덤프와 `--emit-llvm`

바로 위 표의 직접적인 결과가 하나 더 있습니다. **원본 튜토리얼은 입력이 끝날 때
IR 전체를 한 번에 찍는데, 우리 JIT 모드는 그 시점에 찍을 것이 남아 있지
않습니다.** 같은 화면을 만들려면 다른 방법이 필요합니다.

원본은 Ch3과 Ch9에만 이 코드가 있습니다.

```cpp
MainLoop();
TheModule->print(errs(), nullptr);   // Ch3:619, Ch9:1459
```

Ctrl+D로 EOF를 주면 만들어진 IR이 전부 나옵니다. Ch9은 정의를 넣어도 화면에
아무것도 안 찍다가(핸들러에 `FnIR->print`가 없습니다) 마지막에 한꺼번에
쏟아냅니다.

**가르는 기준은 "JIT이 있느냐"가 아니라 "모듈을 넘겨주고 새로 여느냐"입니다.**

| | JIT 실행 | `InitializeModule` | EOF에 찍기 |
| --- | --- | --- | --- |
| Ch3 | 없음 | 1회 | **가능** (`:619`) |
| Ch4–7 | 함 | 구문마다 | 불가 — 줄 자체가 없음 |
| Ch8 | 없음 | 1회 | 가능하나 안 함 (`.o`를 씀) |
| Ch9 | **안 함** | 1회 | **가능** (`:1459`) |

Ch9이 헷갈리는 지점입니다. `KaleidoscopeJIT`을 include하고 객체까지 만들지만
(`:826`, `:1430`), 쓰는 곳은 `TheJIT->getDataLayout()` 한 줄뿐입니다(`:1327`).
`addModule`도 `lookup`도 없어서 **아무것도 실행하지 않습니다.** 디버그 정보와
JIT을 같이 굴리지 않으려고 실행을 들어낸 것이고, 남은 JIT 객체는 DataLayout
공급원일 뿐입니다. 그래서 모듈이 한 번도 넘어가지 않고 EOF까지 쌓입니다.

우리 REPL은 Ch4–7 자리이고, `-c`는 Ch8 자리입니다. 그래서 **각자 대응하는
챕터의 동작을 그대로 따르되, "끝에 IR 전부 보여주기"는 Ch3/Ch9에서 가져왔습니다.**

| | 대응 챕터 | 우리 동작 |
| --- | --- | --- |
| REPL | Ch4–7 | Ch4–7과 동일 + **EOF에 IR 전부** (무조건, 플래그 없음) |
| `-c` | Ch8 | Ch8과 동일 (`Wrote out.o`). IR은 `--emit-llvm`으로 선택 |
| `-c -g` | Ch9 | 위와 같음. `--emit-llvm` 시 `finalize()` 뒤에 찍음 |

`-c`에서 IR을 기본으로 안 찍는 이유는 Ch8이 안 찍기 때문입니다. Ch9은 찍지만
Ch9은 `.o`를 만들지 않습니다 — 둘을 동시에 만족시킬 수 없어서, 오브젝트를
만드는 쪽(Ch8)의 출력을 기본으로 두고 IR은 옵션으로 뺐습니다.

REPL은 실제로 실행하므로 모듈을 넘겨야 하고 EOF에는 남는 것이 없습니다. 그래서
넘길 때마다 IR을 문자열로 모아두었다가 EOF에 함께 찍습니다.

```cpp
// 모듈을 JIT에 넘기기 전에 텍스트만 확보
captureModuleIR(CG, AllIR);
auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());

// EOF
case tok_eof:
  captureModuleIR(CG, AllIR);      // 넘어가지 않고 남은 작업 모듈까지
  if (!AllIR.empty())
    errs() << "\n" << AllIR;
  return 0;
```

EOF에서 한 번 더 모으는 이유는 `extern`만 넣고 끝낸 경우입니다. `extern`은
모듈 경계를 만들지 않으므로 그 선언은 아직 작업 모듈에 남아 있습니다. 비어 있는
모듈은 `Module::empty()`로 걸러 헤더만 있는 덩어리가 나오지 않게 했습니다.

#### 레퍼런스와 대조하기

같은 입력을 Ch7과 우리 것에 넣으면, **프롬프트를 빼고 나면 레퍼런스 출력 전체가
앞부분에 그대로** 나오고 뒤에 IR 덤프만 붙습니다.

```bash
$ ./build.sh ch7.cpp                       # 레퍼런스 빌드
$ printf 'def f(x) x+1;\nf(1);\n' > /tmp/in.ks
$ ./ch7       < /tmp/in.ks > /tmp/ref.txt 2>&1
$ ./build/toy < /tmp/in.ks > /tmp/our.txt 2>&1

$ sed 's/ready> //g' /tmp/ref.txt > /tmp/ref-n.txt
$ sed 's/ready> //g' /tmp/our.txt | head -c $(wc -c < /tmp/ref-n.txt) \
    | cmp - /tmp/ref-n.txt        # 차이 없음
```

`sed`로 프롬프트를 지우는 이유는 **거기 하나만 일부러 다르게 뒀기** 때문입니다.

레퍼런스는 `ready> `를 루프 맨 위에서 찍습니다.

```cpp
// ch7.cpp — MainLoop
while (true) {
  fprintf(stderr, "ready> ");
  switch (CurTok) {
  case ';': getNextToken(); break;      // ← 이 반복도 프롬프트를 하나 쓴다
```

파서가 최상위 `;`를 소비하지 않으므로, 구문 하나를 처리한 뒤 `;`가 반복을 한 번
더 소모하고 프롬프트를 하나 더 냅니다. 그래서 정상 세션인데도 `ready> ready> `가
찍힙니다. 우리는 `;`에서 프롬프트를 건너뛰어 구문당 하나만 나오게 했습니다.

```cpp
// src/main.cpp
switch (P.getCurTok()) {
case ';':
  P.advance();
  continue;                    // ← 프롬프트를 찍지 않고 다음 반복으로
...
}
fprintf(stderr, "ready> ");    // 구문을 처리했을 때만, 한 곳에서
```

같은 입력에서 레퍼런스는 프롬프트 6개, 우리는 3개입니다. **프롬프트를 뺀
나머지는 전부 동일합니다.**

#### 출력이 Ch9과 다른 한 가지

```
ready> Evaluated to 1.909297
ready>
; ModuleID = 'KaleidoscopeJIT'
declare double @sin(double)          ← f의 모듈에 선언이 다시 들어와 있다
define double @f(double %x) { ... }

; ModuleID = 'KaleidoscopeJIT'
define double @__anon_expr() { ... }
declare double @f(double)            ← 여기서도 다시
```

**모듈이 여러 개로 나뉘어 나옵니다.** Ch9은 실행을 하지 않아 하나로 합쳐지지만,
우리는 실제로 JIT 실행을 하므로 넘긴 단위가 그대로 보입니다. 그리고 `declare`가
모듈마다 다시 나타나는 것이 [04-codegen](04-codegen.md) 6.2절의
`getFunction`/`FunctionProtos`가 하는 일입니다 — 모듈이 바뀌었으니 선언을 새로
만들어 준 것입니다.

#### 에러 복구

파싱이 실패하면 두 드라이버 모두 토큰 하나를 버리고 루프를 계속합니다.

```cpp
} else {
  P.advance();   // 에러 복구 — 무한 루프 방지
}
```

이 한 줄이 없으면 같은 토큰에서 계속 실패하며 멈추지 않습니다. 입력 끝에서
실패해도 안전한 이유는 `tok_eof`가 sticky하기 때문입니다
([01-lexer](01-lexer.md) 5절).

차이는 **결과 처리**입니다. JIT은 에러를 찍고 계속 REPL을 돌지만, 컴파일
모드는 `Failed` 플래그를 세워 두었다가 마지막에 오브젝트 파일을 만들지 않고
종료 코드 1로 끝냅니다. 반쯤 만들어진 `.o`가 남으면 안 되기 때문입니다.

### 3.4 JIT 드라이버

```cpp
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
  CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), true);

  fprintf(stderr, "ready> ");
  Parser P(Lex, Ops);

  while (true) {
    switch (P.getCurTok()) {
    case tok_eof:    return 0;
    case ';':        P.advance(); break;
    case tok_def:    /* … */      break;
    case tok_extern: /* … */      break;
    default:         /* 최상위 식 */ break;
    }
  }
}
```

여기서 모든 조각이 조립됩니다. `OperatorTable`을 `main`이 만들어 `Parser`와
`CodeGen`에 각각 참조로 넘기는 것을 볼 수 있습니다
([03-parser](03-parser.md) 4절의 순환 의존 해소).

`ExitOnError`는 LLVM의 도우미로, 오류가 담긴 결과를 받으면 메시지를 찍고
프로그램을 종료합니다. 예제 코드에서 오류 처리를 간결하게 하려는 장치입니다.

### 3.5 모듈 수명 — 함수 정의 vs 최상위 식

**이 코드베이스에서 가장 미묘한 부분입니다.**

**함수 정의(`def`)**:

```cpp
if (Function *FnIR = CG.codegen(*FnAST)) {
  fprintf(stderr, "Read function definition:");
  FnIR->print(errs());

  auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
  ExitOnErr(TheJIT->addModule(std::move(TSM)));     // ← 추적자 없음
  CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), true);
}
```

**최상위 식**:

```cpp
if (CG.codegen(*FnAST)) {
  auto RT = TheJIT->getMainJITDylib().createResourceTracker();   // ← 추적자
  auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
  ExitOnErr(TheJIT->addModule(std::move(TSM), RT));

  CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), true);

  auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
  double (*FP)() = ExprSymbol.toPtr<double (*)()>();
  fprintf(stderr, "Evaluated to %f\n", FP());

  ExitOnErr(RT->remove());        // ← 평가 후 해제
}
```

차이가 결정적입니다.

| | ResourceTracker | 수명 |
| --- | --- | --- |
| `def` | 없음 | **영구** — 나중 식이 호출해야 하므로 |
| 최상위 식 | 있음 | 평가 직후 **제거** |

`ResourceTracker`는 "이 모듈이 쓴 메모리"에 붙이는 꼬리표입니다.
`RT->remove()`로 한 번에 해제할 수 있습니다. 매 입력마다 쌓이는 익명 함수를
치우기 위한 장치입니다.

> **이 부분에서 실제로 버그를 냈습니다.** 처음에는 `def`를 처리할 때 IR만
> 출력하고 JIT에 넘기지 않았습니다. 그러면 정의가 작업 중인 모듈에 남아 있다가,
> 다음 최상위 식이 그 모듈을 통째로(추적자와 함께) 넘기고 평가 후 제거하면서
> **함수 정의까지 같이 사라졌습니다.**
>
> 증상이 특히 고약합니다. 이후 호출은 코드 생성까지는 성공합니다.
> `getFunction`이 `FunctionProtos`에서 프로토타입을 찾아 **선언**을 만들어
> 주기 때문입니다. 실패는 한참 뒤 JIT 심볼 조회에서 납니다.
>
> ```
> def unary-(v) 0-v;
> -(5);        # 정상
> -(7);        # error: Symbols not found: [ unary- ]
> ```
>
> `tests/operators.ks`가 이 회귀를 막고 있습니다.

`double (*FP)()` 는 **함수 포인터** 선언입니다. "인자가 없고 `double`을
반환하는 함수를 가리키는 포인터". JIT이 준 주소를 이 타입으로 해석해서
`FP()`로 호출하면, 방금 컴파일한 기계어가 실행됩니다.

### 3.6 컴파일 드라이버

```cpp
int runCompile(const Options &O) {
  ObjectEmitter::initializeTargets();

  std::string Error;
  auto TM = ObjectEmitter::createHostTargetMachine(Error);
  if (!TM) { errs() << Error << "\n"; return 1; }

  std::ifstream In(O.Input);
  if (!In) { errs() << "could not open " << O.Input << "\n"; return 1; }

  OperatorTable Ops;
  Lexer Lex(In);                 // ← std::cin이 아니라 파일
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
        "Kaleidoscope Compiler", false, "", 0);
    Dbg = std::make_unique<DebugInfo>(*DBuilder, CG.getBuilder(), CU);
    CG.setDebugInfo(Dbg.get());
  }

  Parser P(Lex, Ops);
  bool SawTopLevel = false;
  ...
```

**`Lexer Lex(In)` 한 줄만 다릅니다.** Lexer가 `std::istream&`을 받도록 만든
덕분입니다 ([01-lexer](01-lexer.md) 4.1절). `getchar()`였다면 파일 모드를
위해 Lexer를 고쳐야 했을 것입니다.

`initModule`은 **한 번만** 부릅니다. 모든 함수가 한 모듈에 쌓이고, 마지막에
통째로 오브젝트 파일이 됩니다.

최상위 식은 하나만 허용합니다.

```cpp
if (SawTopLevel) {
  errs() << "only one top-level expression is supported when compiling "
            "(it becomes main)\n";
  Failed = true;
  break;
}
auto FnAST = P.parseTopLevelExpr("main");
```

`main`이라는 이름으로 만들어지므로 두 개면 중복 정의가 됩니다
([03-parser](03-parser.md) 8절).

마지막에:

```cpp
if (O.Debug)
  DBuilder->finalize();          // 메타데이터 마무리 (반드시 필요)

if (!ObjectEmitter::emit(CG.getModule(), *TM, O.Output, Error)) { ... }
```

`finalize()`를 빠뜨리면 디버그 정보가 불완전한 채로 나옵니다.

---

## 4. 전체 조립도

```
main.cpp
   │
   ├── OperatorTable Ops           ← main이 소유. Parser와 CodeGen이 공유
   ├── Lexer Lex(입력스트림)
   ├── Parser P(Lex, Ops)
   ├── CodeGen CG(Ops)
   │      └── DebugInfo *Dbg       ← -g일 때만
   │
   ├── [JIT 모드]     KaleidoscopeJIT
   └── [컴파일 모드]  ObjectEmitter + TargetMachine
```

소유 관계가 전부 `main` 안에서 드러나 있습니다. 원본 튜토리얼에서는 이 전부가
전역 변수라, 무엇이 무엇에 의존하는지 코드만 봐서는 알 수 없었습니다.

---

## 5. 직접 확인해 보기

```bash
# JIT
echo 'def f(x) x*2; f(21);' | ./build/toy

# extern 호출 (ENABLE_EXPORTS 확인)
echo 'extern putchard(c); putchard(72); putchard(73);' | ./build/toy

# 만들어진 IR 전체 (Ch9의 EOF 출력에 해당)
./build/toy -c tests/fib.ks --emit-llvm -o /tmp/fib.o

# REPL은 EOF(Ctrl+D)에 IR을 몰아서 낸다 (플래그 불필요)
printf 'extern sin(x);\ndef f(x) sin(x)+1;\nf(2);\n' | ./build/toy

# 레퍼런스와 대조 — 프롬프트만 빼면 앞부분이 일치해야 한다
./build.sh ch7.cpp
printf 'def f(x) x+1;\nf(1);\n' > /tmp/in.ks
./ch7       < /tmp/in.ks > /tmp/ref.txt 2>&1
./build/toy < /tmp/in.ks > /tmp/our.txt 2>&1
sed 's/ready> //g' /tmp/ref.txt > /tmp/ref-n.txt
sed 's/ready> //g' /tmp/our.txt | head -c $(wc -c < /tmp/ref-n.txt) | cmp - /tmp/ref-n.txt

# 오브젝트 파일
./build/toy -c tests/fib.ks -o /tmp/fib.o
file /tmp/fib.o
nm /tmp/fib.o | grep ' T '        # 만들어진 함수 심볼

# 최상위 식이 main이 되는 것
nm /tmp/fib.o | grep main

# 최상위 식 두 개는 거부
printf 'def g(x) x;\n1;\n2;\n' > /tmp/two.ks
./build/toy -c /tmp/two.ks -o /tmp/two.o
```

---

## 6. 정리

- `CodeGen`은 자기 결과가 JIT으로 갈지 파일로 갈지 모른다. `DataLayout`만 알면 된다
- 드라이버는 **최상위 구문 하나 단위**로 파싱과 코드 생성을 번갈아 한다.
  파일 전체를 담는 AST는 만들어지지 않고, 트리는 구문마다 태어나 죽는다
- 구문과 LLVM 모듈은 다른 층이다. JIT은 구문마다 모듈을 갈고, `-c`는 전부
  한 모듈에 쌓는다 — `initModule`을 언제 부를지가 드라이버 쪽에 있어서 가능하다
- 코드 생성 진입점은 `visit()`이 아니라 `codegen(FunctionAST&)`다.
  `FunctionAST`/`PrototypeAST`는 `ExprAST`가 아니라 방문자 밖에 있다
- 백엔드 코드 생성은 아직 **구식 패스 매니저**를 쓴다 (최적화는 새 것)
- 호스트만 대상으로 하므로 `InitializeNative*`면 충분하다
- `extern "C"` + `ENABLE_EXPORTS` 둘 다 있어야 JIT이 `putchard`를 찾는다
- JIT 모듈 수명: **`def`는 영구(추적자 없음), 최상위 식은 임시(추적자 있음)**.
  헷갈리면 정의가 사라지는 버그가 난다
- 컴파일 모드는 모듈 하나에 전부 쌓고, 최상위 식은 `main`이라 하나만 가능하다
- Lexer가 스트림을 받은 덕에 두 모드가 `Lexer Lex(In)` 한 줄 차이로 갈린다
- REPL은 Ch4–7과 같은 출력을 내고(프롬프트 배치만 예외), 거기에 Ch3/Ch9식 EOF
  IR 덤프가 덧붙는다. 모듈이 넘어가버리므로 넘기기 직전마다 텍스트를 모아둔다
- 프롬프트는 구문당 하나. 레퍼런스는 `;`에도 하나를 써서 `ready> ready> `가 된다
- `-c`는 Ch8과 같이 IR을 안 찍는다 (`--emit-llvm`으로 선택). Ch9은 찍지만 `.o`를
  만들지 않으므로 둘을 동시에 만족시킬 수 없다

---

**문서 끝.** 설계 배경과 원본 튜토리얼과의 차이 전체 목록은
[`../ARCHITECTURE.md`](../ARCHITECTURE.md)에 있습니다.
