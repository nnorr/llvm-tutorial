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
  std::string Input;
  std::string Output = "output.o";
};
```

파싱은 단순한 루프입니다. LLVM에 `cl::opt`라는 옵션 라이브러리가 있지만,
옵션이 4개뿐이라 직접 처리하는 편이 읽기 쉽습니다.

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

### 3.3 JIT 드라이버

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

### 3.4 모듈 수명 — 함수 정의 vs 최상위 식

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

### 3.5 컴파일 드라이버

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
- 백엔드 코드 생성은 아직 **구식 패스 매니저**를 쓴다 (최적화는 새 것)
- 호스트만 대상으로 하므로 `InitializeNative*`면 충분하다
- `extern "C"` + `ENABLE_EXPORTS` 둘 다 있어야 JIT이 `putchard`를 찾는다
- JIT 모듈 수명: **`def`는 영구(추적자 없음), 최상위 식은 임시(추적자 있음)**.
  헷갈리면 정의가 사라지는 버그가 난다
- 컴파일 모드는 모듈 하나에 전부 쌓고, 최상위 식은 `main`이라 하나만 가능하다
- Lexer가 스트림을 받은 덕에 두 모드가 `Lexer Lex(In)` 한 줄 차이로 갈린다

---

**문서 끝.** 설계 배경과 원본 튜토리얼과의 차이 전체 목록은
[`../ARCHITECTURE.md`](../ARCHITECTURE.md)에 있습니다.
