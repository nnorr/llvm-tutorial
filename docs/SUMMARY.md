# Kaleidoscope 재구현 — 학습 정리

LLVM 공식 [Kaleidoscope 튜토리얼](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/)을
모듈 단위로 다시 구현하면서 정리한 내용. 원본은 챕터마다 `toy.cpp` 한 파일에
모든 상태를 전역으로 두는데, 그 구조가 감추고 있던 결합 관계를 드러내는 것이 목표.

- 상세 설계 배경 → `ARCHITECTURE.md`
- 모듈별 코드 설명(한국어) → `docs/README.md`
- Visitor 설계 변천사 → `docs/09-visitor-evolution.md`
- 디버그 정보와 최적화의 관계 → `docs/10-debuginfo-and-optimization.md`

---

## 1. 무엇을 만들었나

컴파일러 프론트엔드 전체 경로. **원본의 어느 챕터도 이 넷을 동시에 갖지 않는다.**


|                | Ch4 | Ch5–7 | Ch8 | Ch9 | 여기  |
| -------------- | :---: | :-----: | :---: | :---: | :---: |
| 함수 최적화 패스      | ✅   | ✅     | ❌   | ❌   | ✅   |
| JIT 실행         | ✅   | ✅     | ❌   | ❌   | ✅   |
| 오브젝트 파일 출력     | ❌   | ❌     | ✅   | ❌   | ✅   |
| 디버그 정보 (DWARF) | ❌   | ❌     | ❌   | ✅   | ✅   |


Ch8은 JIT과 옵티마이저를 **삭제하고** 오브젝트 출력으로 바꾸고, Ch9은 거기에
디버그 정보를 얹는다. 넷을 동시에 지원하려면 백엔드가 분리 가능해야 하는데,
그게 이 재구현의 가장 직접적인 동기가 됐다.

```bash
./build/toy                              # REPL + JIT, EOF에 생성된 IR 전부 출력
./build/toy -c tests/fib.ks -o fib.o     # 네이티브 오브젝트 파일
./build/toy -c tests/fib.ks -g -o fib.o  # + DWARF 디버그 정보
./build/toy --dump-ast < tests/fib.ks    # 파스 트리 출력
```

---

## 2. 컴포넌트

```
                    SourceLocation.h
                     ╱            ╲
                Lexer.h          AST.h ←── ASTVisitor.h
                    ╲            ╱   ╲ ╲
                     ╲          ╱     ╲ ╰──→ ASTDumper.h
                      Parser.h         CodeGen.h ──→ DebugInfo.h
                          ╲               ╱  ╲
                           ╲             ╱    ╲
                        OperatorTable.h        ObjectEmitter.h
                                    ╲          ╱
                                     ╲        ╱   KaleidoscopeJIT.h
                                      ╲      ╱     ╱
                                       main.cpp ──┘
```

**위를 향하는 화살표가 없다.** `AST.h`가 그래프의 바닥이고 LLVM IR 의존이 전혀
없다 — `Value*`도 `IRBuilder`도 `codegen()`도 없다.


| 파일                      | 줄   | 책임                                               |
| ----------------------- | ---: | ------------------------------------------------ |
| `SourceLocation.h`      | 16  | `{Line, Col}`. Lexer가 만들고 AST가 나르고 DebugInfo가 소비 |
| `Lexer.{h,cpp}`         | 216 | 문자 스트림 → 토큰, 위치 추적                               |
| `ASTVisitor.h`          | 66  | CRTP 디스패처. `Kind` 스위치 한 곳                        |
| `AST.h`                 | 279 | 노드 클래스. **순수 데이터**                               |
| `OperatorTable.h`       | 46  | 이항 연산자 우선순위. Parser·CodeGen 공유                   |
| `Parser.{h,cpp}`        | 490 | 재귀 하강 + 우선순위 등반. AST를 만드는 유일한 곳                  |
| `ASTDumper.{h,cpp}`     | 173 | 두 번째 방문자. **LLVM include 0** (자기 파일 기준)          |
| `CodeGen.{h,cpp}`       | 576 | AST → LLVM IR. 패스 파이프라인 소유                       |
| `DebugInfo.{h,cpp}`     | 127 | DWARF 메타데이터. `DIBuilder` 래핑                      |
| `ObjectEmitter.{h,cpp}` | 101 | Module → `.o`, `TargetMachine` 경유                |
| `main.cpp`              | 369 | 인자 파싱 + 두 드라이버                                   |
| `KaleidoscopeJIT.h`     | 105 | **업스트림 그대로** (수정 없음)                             |


### 데이터 흐름

```
                       ┌────────── JIT 모드 (기본) ──────────┐
                       │                                     │
stdin/파일 → Lexer → Parser → AST → CodeGen → Module → KaleidoscopeJIT → 실행
                                      │                      │
                       └────────── 컴파일 모드 (-c) ─────────┘
                                      │
                                 DebugInfo (-g)   ObjectEmitter → output.o
```

---

## 3. 구현에서 배운 것

### 3.1 드라이버는 최상위 구문 단위로 돈다

파일 전체를 담는 AST는 만들어지지 않는다. **구문(`def` / `extern` / 최상위 식)
하나마다 파싱과 코드 생성이 번갈아** 일어나고, 트리는 그때마다 태어나 죽는다.

```cpp
if (auto FnAST = P.parseDefinition()) {   // ← 트리 하나 생성
    CG.codegen(*FnAST);
}                                          // ← 스코프 벗어나며 통째로 해제
```

`FnAST`는 지역 `unique_ptr`이고 트리 전체의 유일한 소유자다. 예외는
`takeProto()` 하나 — 프로토타입만 떼어 `CodeGen::FunctionProtos`로 옮긴다.
트리가 죽어도 나중 모듈에 함수를 다시 **선언**할 수 있어야 하기 때문.

### 3.2 구문과 LLVM 모듈은 다른 층


|         | 구문 4개짜리 `fib.ks` | `initModule` |
| ------- | ---------------- | ------------ |
| JIT 모드  | 모듈 **4개**        | 구문마다         |
| `-c` 모드 | 모듈 **1개**        | 시작할 때 한 번    |


같은 `CodeGen`이 두 정책을 다 돌릴 수 있는 이유는 `initModule`을 **언제 부를지가
드라이버 쪽에 있기** 때문. 원본처럼 `TheModule`이 전역이면 두 경로가 하나의 모듈을
두고 다투게 되고, 실제로 원본은 Ch8에서 JIT을 삭제해 이 충돌을 피한다.

### 3.3 JIT 모듈 수명 — 실제로 낸 버그

```cpp
// def: ResourceTracker 없음 → 영구
ExitOnErr(TheJIT->addModule(std::move(TSM)));

// 최상위 식: ResourceTracker 있음 → 평가 직후 제거
auto RT = TheJIT->getMainJITDylib().createResourceTracker();
ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
...
ExitOnErr(RT->remove());
```

처음엔 `def`를 IR만 출력하고 JIT에 넘기지 않았다. 그러면 정의가 작업 모듈에
남아 있다가 **다음 최상위 식의 추적되는 모듈에 딸려 들어가 함께 삭제**된다.

증상이 고약하다. 이후 호출도 **코드 생성까지는 성공한다** — `getFunction`이
`FunctionProtos`에서 프로토타입을 찾아 선언을 만들어 주기 때문. 실패는 한참 뒤
JIT 심볼 조회에서 난다.

```
def unary-(v) 0-v;
-(5);        # 정상
-(7);        # error: Symbols not found: [ unary- ]
```

`tests/operators.ks`가 이 회귀를 막고 있다.

### 3.4 `alloca` + mem2reg — 가변 변수의 해법

SSA에서는 이름에 두 번 대입할 수 없다. 답은 **변수를 스택에 두고 load/store로
접근**하는 것. 메모리는 SSA 규칙의 대상이 아니다.

```llvm
; 프론트엔드가 만드는 것 — 쉬운 형태
%x = alloca double
store double 5.0, ptr %x
%x1 = load double, ptr %x
%add = fadd double %x1, 1.0

; PromotePass(mem2reg) 이후 — SSA 레지스터
%add = fadd double 5.0, 1.0
```

`**alloca`는 반드시 entry 블록에 넣어야 한다.** mem2reg는 거기 있는 것만
승격시킨다. 다른 곳에 넣으면 최적화가 조용히 아무것도 안 한다.

```cpp
IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                 TheFunction->getEntryBlock().begin());   // 현재 위치가 아니라 entry 맨 앞
```

`if`는 왜 `phi`를 손으로 만드는가? `if`의 결과는 변수가 아니라 **식의 값**이라
스택에 올릴 대상이 아니기 때문. 변수는 스택에 올려두면 mem2reg가 PHI 배치를
대신 해준다 — 이게 Ch7의 핵심 교훈이다.

### 3.5 우선순위 등반 (`parseBinOpRHS`)

`ExprPrec` 인자 하나가 전부다 — **"이 호출이 삼켜도 되는 연산자의 최소
우선순위"**.

```cpp
while (true) {
  int TokPrec = getTokPrecedence();
  if (TokPrec < ExprPrec) return LHS;        // ① 약하면 호출자에게 위임
  int BinOp = CurTok;  getNextToken();       // ② 연산자 삼킴
  auto RHS = parseUnary();                   // ③ 피연산자 하나만
  int NextPrec = getTokPrecedence();         // ④ 다음 연산자를 엿봄
  if (TokPrec < NextPrec)
    RHS = parseBinOpRHS(TokPrec + 1, std::move(RHS));
  LHS = make_unique<BinaryExprAST>(BinOp, move(LHS), move(RHS));  // ⑤
}
```

① 이 **토큰을 소비하지 않는다**는 게 핵심. 반환은 포기가 아니라 "이건 바깥
레벨의 연산자다"라는 위임이고, 호출자의 루프가 같은 토큰을 다시 본다.

`1 + 2 * 3 - 4` 추적:

```
parseBinOpRHS(0, 1)                     ← A
  '+'(20) >= 0    삼킴, RHS=2
  '*'(40) > 20    → 재귀
      parseBinOpRHS(21, 2)              ← B
        '*'(40) >= 21   삼킴, RHS=3
        LHS(B) = (2*3)
        '-'(20) < 21    → ★ 반환         ← 여기서 안 넘기면 2*(3-4)가 된다
  LHS(A) = 1 + (2*3)
  '-'(20) >= 0    삼킴 → (1+(2*3)) - 4
  ';' → -1 < 0    → 반환                  ← 종료도 같은 한 줄이 담당
```

`TokPrec + 1`이 **결합 방향**을 정한다. 같은 우선순위가 재귀에서 거부되므로
바깥 루프가 처리 → 좌결합. `TokPrec` 그대로면 우결합이 된다.

⑤에서 `LHS`가 새 노드의 자식으로 들어가고 새 노드가 `LHS` 자리를 차지한다.
반복마다 트리가 위로 한 층씩 자란다.

### 3.6 사용자 정의 연산자 = 이름을 조작한 평범한 함수

```
def binary | 5 (LHS RHS) ...   →  "binary|" 라는 이름의 함수
```

파서가 이름을 만들고(`parsePrototype`), codegen은 내장 연산자가 아니면 그냥
호출한다.

```cpp
Function *F = getFunction(std::string("binary") + E.getOp());
return Builder->CreateCall(F, Ops2, "binop");
```

`nm`으로 확인하면 심볼이 진짜 `binary|`다. 우선순위 등반 알고리즘은 손댈 게
없다 — `getTokPrecedence()`가 `OperatorTable`을 볼 뿐이므로.

---

## 4. 설계 결정

### 4.1 `OperatorTable` — 순환 의존 끊기

`FunctionAST`의 codegen이 우선순위 표를 **쓴다**. 사용자 정의 연산자는 정의가
코드 생성돼야 파서가 쓸 수 있기 때문. 즉 CodeGen이 파서 상태를 변경해야 하는
진짜 순환인데, 양쪽이 전역을 만지던 원본에서는 보이지 않던 것이다.

```
Parser  ──읽기──▶  OperatorTable  ◀──쓰기──  CodeGen
```

`main`이 소유하고 양쪽에 참조로 넘긴다. 어느 쪽도 소유하지 않고 서로 의존하지도
않는다.

### 4.2 `AssignExprAST` — 잘못된 상태를 표현 불가능하게

원본은 `=`를 `BinaryExprAST`로 파싱하고 codegen에서 왼쪽을 `static_cast` 한다.
`static_cast`는 null을 내지 않으므로 뒤따르는 null 검사는 **죽은 코드**다.

"`=`의 왼쪽은 식별자"는 **문법 규칙**이므로 파서에서 강제한다.

```cpp
auto *LHSVar = llvm::dyn_cast<VariableExprAST>(LHS.get());
if (!LHSVar)
  return logError("destination of '=' must be a variable");
LHS = std::make_unique<AssignExprAST>(BinLoc, LHSVar->getName(), std::move(RHS));
```

이름을 문자열로 직접 들고 있으니 codegen에는 캐스팅도 실패 경로도 없다.
"변수가 아닌 것에 대입"이 *거부되는* 게 아니라 *표현 불가능*해진다.

레퍼런스와 다른 AST 노드는 이 하나뿐이다.

### 4.3 `Lexer`가 `std::istream&`을 받는다

원본은 `getchar()`로 stdin에 하드코딩돼 있다. 스트림을 받으면 같은 렉서가
stdin(JIT)과 파일(`-c`)을 모두 처리하고, `std::istringstream`으로 단위 테스트도
된다.

```cpp
Lexer Lex(std::cin);   // REPL
Lexer Lex(In);         // -c, std::ifstream
Lexer Lex(SS);         // 테스트
```

`lexer_tests`가 **LLVM 라이브러리를 하나도 링크하지 않는** 것이 그 결과다.

### 4.4 LLVM 스타일 RTTI (`Kind` + `classof`)

LLVM은 보통 `-fno-rtti`로 빌드되므로 `dynamic_cast`를 못 쓴다. 대신 **관용**이
있다 — 상속할 베이스도 매크로도 없고, `llvm/Support/Casting.h`의 템플릿이
요구하는 계약은 하나뿐이다.

> `T`에 `static bool classof(const Base *)`가 있을 것.

```cpp
enum ExprASTKind { Expr_Number, Expr_Variable, ..., Expr_Var };
const ExprASTKind Kind;                    // 생성자가 박고 const

static bool classof(const ExprAST *E) { return E->getKind() == Expr_Number; }
```

`static`이라 vtable을 늘리지 않고, 태그 비교 한 번이라 `dynamic_cast`의 그래프
순회보다 훨씬 싸다. MLIR의 Toy AST도 같은 모양이라 그대로 이어진다.

원래는 `parseBinOpRHS`의 `dyn_cast` 하나 때문에 넣었는데, 나중에 방문자를
`Kind` 스위치로 바꾸면서 재활용됐다.

---

## 5. Visitor 패턴과 CRTP

AST 순회 방식은 **세 판**을 거쳤다. 각 단계가 앞 단계의 구체적인 불편 하나를
없앤다.


|           | 분기 방식                   | 상태 위치     | 반환 타입                | AST가 아는 것 |
| --------- | ----------------------- | --------- | -------------------- | --------- |
| **1판** 원본 | 노드의 `virtual codegen()` | **전역 변수** | `Value*`             | LLVM 전부   |
| **2판**    | vtable 2회 (이중 디스패치)     | 방문자 멤버    | `void` + `Result` 멤버 | 방문자의 존재   |
| **3판** 현재 | `Kind` 스위치 + CRTP       | 방문자 멤버    | `RetTy` (자유)         | **아무것도**  |


### 5.1 1판의 문제 — 전역은 결과이지 원인이 아니다

```cpp
class NumberExprAST : public ExprAST {
public:
  Value *codegen() override;    // ← 인자가 없다
};
```

`codegen()`이 동작하려면 `IRBuilder`와 심볼 테이블이 필요한데 받을 방법이 없다.
그래서 전부 전역이 된다. **전역 변수가 생긴 근본 원인이 이 설계다.**

결과: 인스턴스를 둘 만들 수 없어 JIT과 오브젝트 출력이 공존 불가 → 원본은 Ch8에서
JIT을 삭제한다.

### 5.2 2판 — 교과서적 Visitor, 그리고 남은 문제

```cpp
class ASTVisitor {
  virtual void visit(NumberExprAST &E) = 0;   // ... 9개
};
// AST.h — 노드마다 한 줄
void accept(ASTVisitor &V) override { V.visit(*this); }
```

전역은 사라졌다. 하지만 `visit()`이 값을 반환할 수 없다 — 반환하려면
`ASTVisitor.h`가 `llvm::Value`를 알아야 하고, `AST.h`가 그걸 include하므로
AST 전체가 LLVM에 묶인다.

그래서 `**Result` 멤버**라는 우회가 필요했다.

```cpp
llvm::Value *Result = nullptr;

llvm::Value *CodeGen::codegenExpr(ExprAST &E) {
  Result = nullptr;
  E.accept(*this);
  return Result;
}

// 노드 처리 — 실패 경로마다 두 줄
void CodeGen::visit(BinaryExprAST &E) {
  Value *L = codegenExpr(E.getLHS());
  if (!L) { Result = nullptr; return; }
  ...
  case '+': Result = Builder->CreateFAdd(L, R, "addtmp"); return;
```

`CodeGen.cpp` 한 파일에 `Result = ...`가 **33곳**. 이건 게으름이 아니라
`**accept()`가 가상 함수라는 사실의 직접적 귀결**이다 — 가상 함수는 반환 타입을
방문자마다 다르게 할 수 없다. 선택지는 둘뿐이고 중간이 없다.

1. `accept()` 유지 → 반환값은 멤버에 담는다
2. `accept()` 폐기, `Kind`로 스위치 → 반환 타입 자유

### 5.3 3판 — CRTP란 무엇인가

**CRTP** (Curiously Recurring Template Pattern): 자식이 부모에게 **자기 자신을
템플릿 인자로 넘기는** 기법.

```cpp
template <typename Derived, typename RetTy = void> class ASTVisitor {
  Derived &derived() { return *static_cast<Derived *>(this); }
  //                    ↑ 부모가 자식으로 되돌아간다

public:
  RetTy visit(ExprAST &E) {
    switch (E.getKind()) {
    case ExprAST::Expr_Number:
      return derived().visitNumber(llvm::cast<NumberExprAST>(E));
    // ... 9개
    }
    llvm_unreachable("unknown ExprASTKind");    // default: 없음 → -Wswitch가 잡는다
  }
};

class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> {
  //                              ^^^^^^^ 자기 자신을 넘긴다
  friend class ASTVisitor<CodeGen, llvm::Value *>;   // private 훅 호출 허용
  llvm::Value *visitNumber(NumberExprAST &E);
  ...
};
```

**핵심: `derived().visitNumber(...)`는 가상 함수가 아니라 평범한 함수 호출이다.**
`Derived`가 컴파일 타임에 확정돼 있으므로 vtable을 타지 않고 인라인도 된다.
"가상 함수 없는 다형성"이라고 부르는 이유.

얻은 것:

**(1) 반환 타입이 돌아왔다.** 템플릿이라 인스턴스가 각각 만들어지므로 방문자마다
다른 `RetTy`를 가질 수 있다.

```cpp
class CodeGen   : public ASTVisitor<CodeGen, llvm::Value *> { ... };
class ASTDumper : public ASTVisitor<ASTDumper> { ... };   // RetTy = void
```

```cpp
// 3판 — 위 2판 코드와 비교
Value *CodeGen::visitBinary(BinaryExprAST &E) {
  Value *L = visit(E.getLHS());
  Value *R = visit(E.getRHS());
  if (!L || !R)
    return nullptr;
  switch (E.getOp()) {
  case '+': return Builder->CreateFAdd(L, R, "addtmp");
```

`Result` 멤버, `codegenExpr` 우회, 실패 경로의 짝이 전부 사라졌다.
`CodeGen.cpp` 196줄 수정, **순 -41줄**.

**(2) `AST.h`가 방문자를 모른다.** `accept()` 10줄과 `#include "ASTVisitor.h"`가
사라지고 의존 방향이 뒤집혔다.

```
2판:  AST.h  ──include──▶  ASTVisitor.h
3판:  ASTVisitor.h  ──include──▶  AST.h
```

대가:

- 노드 추가 시 컴파일 **에러**가 아니라 `-Wswitch` **경고**로 잡힌다 (한 단계 늦다)
- `ASTVisitor`가 템플릿이라 방문자를 **다형적으로 담을 수 없다**
- 교과서적인 `accept()`/`visit()` 모양에서 멀어졌다 (학습 목적이라면 실제 비용)

이 저장소는 `main.cpp`가 `CodeGen`과 `ASTDumper`를 구체 타입으로 쓰므로 두 번째
대가는 실제로 치르지 않는다. Clang이 `StmtVisitor`에서 쓰는 것과 같은 모양이다.

### 5.4 Expression Problem — 공짜가 아니다


|                | 새 **연산** 추가 | 새 **노드 타입** 추가  |
| -------------- | ----------- | --------------- |
| 1판 (노드에 메서드)   | 노드 9개 전부 수정 | 클래스 하나 작성       |
| 2·3판 (visitor) | 클래스 하나 작성   | 스위치 + 방문자 전부 수정 |


**어느 쪽도 둘 다 싸게 만들지 못한다.** 연산 축을 싸게 만드는 대신 노드 축을
비싸게 만든 거래다.

Kaleidoscope에서는 옳은 거래다 — 문법이 Ch7에서 동결됐으니 **비용을 치를 축을
이 프로젝트는 밟지 않는다.** 반면 이득이 있는 "새 연산 추가" 축은 이미 두 번
썼다(`CodeGen`, `ASTDumper`). `ASTDumper`는 `AST.h`를 한 줄도 건드리지 않고
추가됐고, 자기 파일에 LLVM include가 한 줄도 없다 — `<ostream>`과 `AST.h`,
`ASTVisitor.h`뿐이다. (전이적으로는 `Casting.h`/`ErrorHandling.h` 두 개가
딸려오지만 둘 다 header-only 유틸이고, 링크되는 LLVM 라이브러리는 없다.)




---

## 6. 테스트 — 두 층

```bash
ctest --test-dir build --output-on-failure     # 4개 스위트
```

| 스위트 | 무엇을 |
| --- | --- |
| `lexer` | 단위 테스트 87개. LLVM 링크 없음 |
| `jit_fib`, `jit_operators` | 종단간. JIT 결과값이 89 / 30인지 |
| `lit` | **IR 테스트 10개** |

앞의 셋은 "컴파일러가 도는가"를 본다. 프론트엔드의 실제 계약인 **생성된 IR**은
`lit` + `FileCheck`가 본다 — `llvm/test`와 같은 방식으로, 테스트 파일 하나가
자기 `RUN:` 줄과 `CHECK:` 기대값을 같이 들고 있다.

```
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def fib(n) var a = 0, b = 1, c in ... ;

# CHECK-LABEL: define double @fib(double %n)
# CHECK-NOT:     alloca
# CHECK:         phi double
```

`test/codegen`은 mem2reg 승격과 bool 왕복 접힘을, `test/debuginfo`는 `-g`의
DWARF 메타데이터와 **`-g`에서 alloca가 살아남는 것**을, `test/driver`는 최상위
식이 `main`이 되는 것과 오류 경로를 검사한다. 테스트 추가는 파일 하나 놓는
것으로 끝이고 재컴파일이 없다.

효과는 확인됐다. 최적화를 강제로 끄거나 `-g`에서 강제로 켜는 변이를 넣으면
정확히 해당 테스트만 깨진다. 그리고 `test/driver/bad-number.ks`를 쓰다가
`Lexer::hadError()`가 없어서 잘못된 리터럴에도 `.o`가 나오던 문제를 찾았다.

도구 배선과 `CHECK` 지시자는 [11. 테스트](11-testing.md)에서 다룬다.
