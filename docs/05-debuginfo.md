# 05. DebugInfo — DWARF 디버그 정보

**파일**: `include/DebugInfo.h`, `src/DebugInfo.cpp`
**원본 튜토리얼**: Chapter 9 — *Adding Debug Information*

---

## 1. 무엇을 위한 것인가

디버거(gdb, lldb)로 프로그램을 멈추고 들여다보려면, 기계어와 소스 코드를
연결하는 정보가 필요합니다.

```
기계어 주소 0x1a  ←→  fib.ks 3번째 줄 11번째 칸
레지스터 어딘가  ←→  변수 n
```

이 대응표의 표준 형식이 **DWARF**이고, 실행 파일 안의 `.debug_*` 섹션에
들어갑니다. LLVM에서는 IR에 **메타데이터**를 붙이는 방식으로 만듭니다.

```llvm
define double @fib(double %n) !dbg !8 {      ; ← 이 함수는 !8 정보를 가짐
entry:
  %addtmp = fadd double %a, %b, !dbg !15     ; ← 이 명령어는 !15 위치에서 왔음
```

`!8`, `!15` 가 메타데이터 참조입니다.

확인:

```bash
$ ./build/toy -c tests/fib.ks -g -o /tmp/fib.o
$ llvm-dwarfdump --debug-info /tmp/fib.o | head -20
0x0000000b: DW_TAG_compile_unit
              DW_AT_producer  ("Kaleidoscope Compiler")
              DW_AT_name      ("tests/fib.ks")
0x0000002a:   DW_TAG_subprogram
                DW_AT_name      ("fib")
                DW_AT_decl_line (2)
0x00000043:     DW_TAG_formal_parameter
                  DW_AT_name    ("n")
```

---

## 2. `-g`가 `-c`를 요구하는 이유

이 저장소에서 디버그 정보는 **파일을 컴파일할 때만** 만들어집니다.

```bash
$ ./build/toy -g < tests/fib.ks
-g requires -c: debug info describes a source file, which the interactive
JIT does not have
```

이유가 셋입니다.

1. **DWARF는 디스크의 파일을 가리킵니다.** REPL에는 파일이 없습니다.
   원본 튜토리얼도 이 벽에 부딪혀 파일 이름을 `"fib.ks"`로 하드코딩하고,
   주석으로 "stdin을 리다이렉트하는 중이라 실제 위치를 쓰고 싶지만 못 한다"고
   인정합니다.
2. **JIT의 모듈은 사라집니다.** 최상위 식마다 모듈을 만들고 평가 후 해제하므로
   ([06-backend-and-driver](06-backend-and-driver.md) 참고), 그것을 설명하는
   메타데이터는 이미 없는 코드를 가리키게 됩니다.
3. JIT 코드를 디버깅하려면 GDB JIT 등록 인터페이스라는 별도 장치가 필요합니다.

그래서 원본 튜토리얼 Ch9는 **JIT을 포기하고** 파일 컴파일 방식으로 바꿉니다.
이 저장소는 두 모드를 모두 유지하고, 디버그 정보를 `-c` 쪽에만 붙입니다.

이것이 모듈로 나눈 덕을 본 부분입니다. `CodeGen`이 `DebugInfo *`를 **널 가능한
포인터**로 들고 있어서, JIT 모드에서는 그냥 꺼 두면 됩니다.

```cpp
if (Dbg)                     // -g가 아니면 통째로 건너뜀
  Dbg->emitLocation(&E);
```

---

## 3. 클래스 구조

```cpp
class DebugInfo {
  llvm::DIBuilder &DBuilder;              // 메타데이터 생성기
  llvm::IRBuilder<> &Builder;             // 위치를 붙일 대상
  llvm::DICompileUnit *TheCU;             // 번역 단위(파일) 정보
  llvm::DIType *DblTy = nullptr;          // double 타입 정보 (캐시)

  std::vector<llvm::DIScope *> LexicalBlocks;   // 스코프 스택

public:
  DebugInfo(llvm::DIBuilder &DBuilder, llvm::IRBuilder<> &Builder,
            llvm::DICompileUnit *TheCU)
      : DBuilder(DBuilder), Builder(Builder), TheCU(TheCU) {}

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
```

`DI` 접두사가 붙은 타입들이 DWARF 메타데이터입니다.

| 타입 | 뜻 |
| --- | --- |
| `DIBuilder` | 메타데이터를 만드는 도우미 (`IRBuilder`의 디버그 정보판) |
| `DICompileUnit` | 컴파일 단위 = 소스 파일 하나 |
| `DISubprogram` | 함수 하나의 디버그 정보 |
| `DIType` | 타입 정보 |
| `DIScope` | 스코프 (파일, 함수, 블록 …) |
| `DILocation` | 줄·칸 위치 |

### 왜 `DIBuilder`를 참조로 받는가

`DIBuilder`는 **모듈이 이미 존재해야** 만들 수 있습니다. 그래서 생성 순서가
정해져 있습니다.

```cpp
// main.cpp
CG.initModule(...);                                  // 1. 모듈 생성
DBuilder = std::make_unique<DIBuilder>(CG.getModule());  // 2. DIBuilder
DICompileUnit *CU = DBuilder->createCompileUnit(...);    // 3. 컴파일 단위
Dbg = std::make_unique<DebugInfo>(*DBuilder, CG.getBuilder(), CU);  // 4.
CG.setDebugInfo(Dbg.get());                          // 5. CodeGen에 연결
```

`DebugInfo`가 `DIBuilder`를 소유하지 않고 참조만 하는 것은 이 순서를 코드에
드러내기 위해서입니다. 원본 튜토리얼은 둘 다 전역이라 순서가 보이지 않습니다.

---

## 4. 구현 읽기

### 4.1 `getDoubleTy` — 타입 정보 캐시

```cpp
DIType *DebugInfo::getDoubleTy() {
  if (DblTy)
    return DblTy;

  DblTy = DBuilder.createBasicType("double", 64, dwarf::DW_ATE_float);
  return DblTy;
}
```

Kaleidoscope의 타입은 `double` 하나뿐이라 한 번 만들어 재사용합니다.

- `64` — 비트 수
- `dwarf::DW_ATE_float` — DWARF의 "부동소수점" 인코딩 상수

### 4.2 `emitLocation` — 명령어에 위치 붙이기

```cpp
void DebugInfo::emitLocation(const ExprAST *AST) {
  if (!AST)
    return Builder.SetCurrentDebugLocation(DebugLoc());   // 위치 지우기

  DIScope *Scope;
  if (LexicalBlocks.empty())
    Scope = TheCU;
  else
    Scope = LexicalBlocks.back();

  Builder.SetCurrentDebugLocation(DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}
```

동작 방식이 중요합니다. **명령어 하나하나에 직접 붙이는 것이 아니라,
`IRBuilder`의 "현재 위치"를 설정합니다.** 이후 `Builder->CreateXxx`로 만드는
모든 명령어에 그 위치가 자동으로 붙습니다.

그래서 CodeGen의 각 `visit`이 이렇게 생겼습니다.

```cpp
void CodeGen::visit(BinaryExprAST &E) {
  if (Dbg)
    Dbg->emitLocation(&E);      // "여기부터는 이 노드의 위치"
  ...                           // 이후 만드는 명령어들이 그 위치를 물려받음
}
```

여기서 [01-lexer](01-lexer.md)에서 Lexer가 기록하고 Parser가 노드에 넣어 준
`SourceLocation`이 쓰입니다. **Ch1의 줄·칸 추적이 Ch9에 와서 결실을 맺는
지점**입니다.

**`AST == nullptr`일 때 위치를 지우는 것**도 의미가 있습니다.
함수 시작부의 명령어들(인자를 스택에 복사하는 부분)에 위치가 없으면 디버거가
그것을 **프롤로그**로 간주하고 건너뜁니다. 그래서 함수에 중단점을 걸면
인자 복사 코드가 아니라 실제 본문 첫 줄에서 멈춥니다.

`CodeGen::codegen(FunctionAST&)`에 이 호출이 있습니다.

```cpp
Dbg->emitLocation(nullptr);        // 프롤로그 시작
... 인자를 alloca에 저장 ...
Dbg->emitLocation(&F.getBody());   // 여기부터 본문
```

### 4.3 `createFunctionType` — 함수 시그니처

```cpp
DISubroutineType *DebugInfo::createFunctionType(unsigned NumArgs) {
  SmallVector<Metadata *, 8> EltTys;
  DIType *DblTy = getDoubleTy();

  EltTys.push_back(DblTy);              // [0] 반환 타입

  for (unsigned I = 0; I != NumArgs; ++I)
    EltTys.push_back(DblTy);            // [1..] 인자 타입들

  return DBuilder.createSubroutineType(DBuilder.getOrCreateTypeArray(EltTys));
}
```

**첫 원소가 반환 타입**이고 나머지가 인자입니다. DWARF의 규약입니다.

`SmallVector<T, N>`은 LLVM이 제공하는 벡터로, 원소가 N개 이하면 힙 할당 없이
스택에 담습니다. 성능 최적화이며 `std::vector`처럼 쓰면 됩니다.

### 4.4 `createFunction` — 함수 디버그 정보

```cpp
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
```

- `Name` — 소스에서의 이름
- 세 번째 `StringRef()` — 링크 이름(mangled name). Kaleidoscope는 이름 변형이
  없어 비워 둡니다
- `LineNo` — 선언된 줄
- `ScopeLine` — 함수 본문이 시작하는 줄 (여기서는 같게 둠)
- `FlagPrototyped` — 인자 타입 정보가 있음
- `SPFlagDefinition` — 선언이 아니라 정의

`CodeGen`이 이것을 받아 함수에 붙입니다.

```cpp
SP = Dbg->createFunction(P.getName(), LineNo, TheFunction->arg_size());
TheFunction->setSubprogram(SP);
Dbg->pushScope(SP);          // 이 함수 안에서 만들어질 위치들의 스코프
```

### 4.5 `declareParameter` — 인자 변수

```cpp
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
```

`insertDeclare`가 **"이 변수는 이 메모리 위치에 있다"** 를 기록합니다.
디버거가 `print n` 했을 때 어디를 읽어야 하는지 알려주는 정보입니다.

`ArgIdx`는 **1부터** 시작합니다. CodeGen이 `++ArgIdx`를 전위 증가로 넘기는
이유가 이것입니다.

```cpp
Dbg->declareParameter(SP, Arg.getName(), ++ArgIdx, LineNo, Alloca, ...);
//                                       ^^ 0에서 시작하니 먼저 1 증가
```

여기서도 [04-codegen](04-codegen.md) 5절의 `alloca`가 다시 등장합니다.
변수가 메모리에 있기 때문에 "어디 있는지" 말할 수 있는 것입니다.

### 4.6 스코프 스택

```cpp
void pushScope(llvm::DIScope *Scope) { LexicalBlocks.push_back(Scope); }
void popScope() { LexicalBlocks.pop_back(); }
```

`emitLocation`이 `LexicalBlocks.back()`을 현재 스코프로 씁니다. 함수에
들어갈 때 push, 나올 때 pop 합니다.

`CodeGen::codegen(FunctionAST&)`은 **성공·실패 모든 경로에서** pop 합니다.
빠뜨리면 스택이 어긋나 이후 위치 정보가 전부 틀어집니다.

---

## 5. 최적화를 끄는 이유

```cpp
// main.cpp
CG.initModule(O.Input, TM->createDataLayout(), /*Optimize=*/!O.Debug);
```

`-g`이면 최적화 패스를 돌리지 않습니다.

- 원본 튜토리얼 Ch9도 패스를 전혀 실행하지 않습니다
- 최적화는 명령어를 재배치·병합·삭제하므로, 줄 번호가 소스와 어긋나
  디버거에서 "실행이 이리저리 튀는" 현상이 생깁니다
- 무엇보다 mem2reg가 `alloca`를 없애면, 4.5절의 `insertDeclare`가 가리킬
  주소 자체가 사라집니다

실제 컴파일러의 `-O0 -g` 조합과 같은 이유입니다. 다만 clang/gcc는 `-O2 -g`도
지원하는데, 그건 `dbg_declare` 대신 `dbg_value`를 쓰기 때문입니다 →
[10. 디버그 정보와 최적화](10-debuginfo-and-optimization.md)

---

## 6. 직접 확인해 보기

```bash
./build/toy -c tests/fib.ks -g -o /tmp/fib.o

# DWARF 섹션이 생겼는지
readelf -S /tmp/fib.o | grep debug

# 함수와 인자 정보
llvm-dwarfdump --debug-info /tmp/fib.o | grep -E 'DW_TAG|DW_AT_name|decl_line'

# 줄 번호 표 — 주소와 소스 줄·칸의 대응
llvm-dwarfdump --debug-line /tmp/fib.o | tail -20

# -g 없이 만들면 debug 섹션이 없음
./build/toy -c tests/fib.ks -o /tmp/nodbg.o
readelf -S /tmp/nodbg.o | grep -c debug        # 0
```

줄 번호 표를 보면 `prologue_end` 표시가 있는데, 4.2절에서 위치를 지운 결과입니다.

---

## 7. 정리

- DWARF = 기계어와 소스를 잇는 표준 형식. LLVM에서는 IR 메타데이터로 만든다
- `DIBuilder`가 메타데이터를, `IRBuilder`가 명령어를 만든다. 짝을 이룬다
- `emitLocation`은 명령어에 직접 붙이지 않고 **빌더의 현재 위치를 설정**한다
- `emitLocation(nullptr)`로 프롤로그를 표시해 디버거가 건너뛰게 한다
- 변수 위치는 `alloca` 덕분에 표현 가능하다 (Ch7과 Ch9가 연결되는 지점)
- Lexer의 줄·칸 추적(Ch1)이 여기서 쓰인다
- `-g`는 `-c`에서만. DWARF는 디스크의 파일을 전제로 한다
- 널 가능한 `DebugInfo *` 하나로 두 모드를 공존시켰다 (원본은 둘 중 하나만 가능)

**다음**: [06. Backend & Driver](06-backend-and-driver.md) — 오브젝트 파일 출력과 전체 구동
