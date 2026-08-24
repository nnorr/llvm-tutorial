# 02. AST — 프로그램을 트리로

**파일**: `include/AST.h`, `include/ASTVisitor.h`, `include/ASTDumper.h`, `src/ASTDumper.cpp`
**원본 튜토리얼**: Chapter 2 — *Implementing a Parser and AST*

---

## 1. AST가 무엇인가

**A**bstract **S**yntax **T**ree, 추상 구문 트리. 토큰의 나열을
**구조**로 바꾼 것입니다.

토큰만으로는 계산 순서를 알 수 없습니다.

```
토큰:  [1] [+] [2] [*] [3]
```

`(1+2)*3` 인지 `1+(2*3)` 인지 토큰 나열에는 정보가 없습니다. 트리는 그 답을
모양 자체로 담습니다.

```
       Binary '+'
        ├── Number 1
        └── Binary '*'
              ├── Number 2
              └── Number 3
```

자식이 먼저 계산되므로 `2*3`이 먼저입니다. **괄호도, 우선순위도 트리에서는
사라집니다** — 이미 구조에 반영됐기 때문입니다.

직접 볼 수 있습니다.

```bash
$ echo '1 + 2 * 3;' | ./build/toy --dump-ast
Function
  Prototype __anon_expr () @1
  Body:
    Binary '+' @1:3
      LHS:
        Number 1 @1:1
      RHS:
        Binary '*' @1:7
          LHS:
            Number 2 @1:5
          RHS:
            Number 3 @1:9
```

---

## 2. 노드 종류

`ExprAST`가 모든 **식(expression)** 노드의 부모입니다.

| 클래스 | 담당 문법 | 예 |
| --- | --- | --- |
| `NumberExprAST` | 숫자 리터럴 | `1.0` |
| `VariableExprAST` | 변수 참조 | `x` |
| `UnaryExprAST` | 단항 연산자 | `!x` |
| `BinaryExprAST` | 이항 연산자 | `a + b` |
| `AssignExprAST` | 대입 | `x = 5` |
| `CallExprAST` | 함수 호출 | `foo(1, 2)` |
| `IfExprAST` | 조건 | `if a then b else c` |
| `ForExprAST` | 반복 | `for i = 1, i < n in body` |
| `VarExprAST` | 지역 변수 선언 | `var a = 1 in body` |

식이 **아닌** 두 개가 더 있고, 이들은 `ExprAST`를 상속하지 않습니다.

| 클래스 | 뜻 |
| --- | --- |
| `PrototypeAST` | 함수 시그니처 — 이름과 인자 이름들 |
| `FunctionAST` | 함수 정의 — 프로토타입 + 본문 |

> Kaleidoscope에는 **문(statement)이 없습니다.** `if`도 값을 돌려주는 식입니다.
> `if a then 1 else 2` 는 그 자체로 `1` 또는 `2`라는 값입니다. 이 성질이
> [04-codegen](04-codegen.md)에서 PHI 노드로 곧장 이어집니다.

---

## 3. 이 저장소의 핵심 차이 — 노드는 순수한 데이터

원본 튜토리얼은 각 노드에 **코드 생성 함수를 직접 달아 둡니다.**

```cpp
// 원본 튜토리얼
class NumberExprAST : public ExprAST {
  double Val;
public:
  Value *codegen() override;    // ← 노드가 LLVM IR을 직접 만든다
};
```

문제는 `codegen()`이 동작하려면 LLVM의 `IRBuilder`, 심볼 테이블 등이
필요한데, 노드는 그걸 받을 방법이 없다는 점입니다. 그래서 튜토리얼은 그것들을
전부 **전역 변수**로 만듭니다.

```cpp
// 원본 튜토리얼 — 전역 변수들
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, AllocaInst *> NamedValues;
```

**전역 변수가 생긴 근본 원인이 바로 이 설계입니다.**

이 저장소는 반대로 갑니다. 노드는 데이터만 들고, 아무것도 계산하지 않습니다.

```cpp
class NumberExprAST : public ExprAST {
  double Val;
public:
  NumberExprAST(SourceLocation Loc, double Val)
      : ExprAST(Expr_Number, Loc), Val(Val) {}

  double getVal() const { return Val; }                     // 값을 읽는 통로

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Number; }
};
```

`codegen()`이 없습니다. **그 자리에 아무것도 없습니다.** 생성자, getter,
`classof` — 전부 이 노드 자신에 대한 것뿐이고, 소비자를 위해 붙은 것은 하나도
없습니다. `AST.h`는 `ASTVisitor.h`를 include조차 하지 않습니다(방향이 반대).

---

## 4. Visitor 패턴

### 4.1 해결하려는 문제

`ExprAST *E`를 하나 받았을 때, 이게 숫자인지 함수 호출인지에 따라 다르게
처리해야 합니다. 소박한 방법은 이렇습니다.

```cpp
// 이렇게 하고 싶지 않다
if (타입이 NumberExprAST) { ... }
else if (타입이 VariableExprAST) { ... }
else if (...)   // 노드 종류가 늘 때마다 모든 if 사슬을 고쳐야 함
```

Visitor 패턴은 이 분기를 **한 곳에 몰아넣고**, 각 소비자는 노드별 처리만
쓰게 합니다.

### 4.2 구조 — CRTP + `Kind` 스위치

`ASTVisitor.h` 전체가 사실상 함수 하나입니다.

```cpp
template <typename Derived, typename RetTy = void> class ASTVisitor {
  Derived &derived() { return *static_cast<Derived *>(this); }

public:
  RetTy visit(ExprAST &E) {
    switch (E.getKind()) {
    case ExprAST::Expr_Number:
      return derived().visitNumber(llvm::cast<NumberExprAST>(E));
    case ExprAST::Expr_Binary:
      return derived().visitBinary(llvm::cast<BinaryExprAST>(E));
    // ... 9개
    }
    llvm_unreachable("unknown ExprASTKind");
  }
};
```

읽을 것이 세 가지입니다.

**(1) `E.getKind()` 스위치.** 5절의 `Kind` 판별자를 그대로 씁니다. 4.1에서
"쓰고 싶지 않다"던 `if` 사슬이 사실 여기 있습니다 — 다만 **딱 한 번, 한 곳에만**
있고, 소비자는 이걸 다시 쓰지 않습니다. `default:`를 일부러 두지 않아서
`ExprASTKind`에 항목을 추가하면 `-Wswitch`가 여기를 지목합니다.

**(2) `Derived` 템플릿 인자 — CRTP.** `CodeGen`이 자기 자신을 부모에게 알려주는
구조입니다.

```cpp
class CodeGen : public ASTVisitor<CodeGen, llvm::Value *> { ... };
//                                ^^^^^^^ 자기 자신을 넘긴다
```

부모가 `static_cast<Derived*>(this)`로 자식을 되찾으므로, `derived().visitNumber(...)`
는 **가상 함수가 아니라 평범한 함수 호출**입니다. vtable을 타지 않습니다.

**(3) `RetTy`.** 이게 CRTP를 쓰는 이유입니다 — 4.4에서 다룹니다.

### 4.3 디스패치 과정

```cpp
CodeGen CG(...);
ExprAST &E = /* 실제로는 NumberExprAST */;
Value *V = CG.visit(E);
```

1. `CG.visit(E)` — 상속받은 `ASTVisitor<CodeGen, Value*>::visit`이 불립니다.
2. `E.getKind()`가 `Expr_Number`이므로 그 `case`로 갑니다.
3. `llvm::cast<NumberExprAST>(E)`로 타입을 좁히고 —
   `Kind`를 이미 확인했으니 안전합니다 (5절 참고).
4. `derived().visitNumber(...)` — `CodeGen::visitNumber`가 직접 호출됩니다.

분기가 런타임에 일어나는 곳은 **2번의 스위치 하나뿐**입니다.

> **이전 버전은 달랐습니다.** 예전에는 노드마다 `virtual void accept(ASTVisitor&)`가
> 있었고, 디스패치가 vtable을 두 번 타는 *이중 디스패치(double dispatch)* 였습니다.
> 왜 바꿨는지는 [09. Visitor 설계 변천](09-visitor-evolution.md)에 정리했습니다.

### 4.4 반환 타입 — `RetTy`

`CodeGen`은 `llvm::Value*`를 만들어야 하고, `ASTDumper`는 출력만 하면 됩니다.
같은 디스패처를 쓰면서 반환 타입이 다릅니다.

```cpp
class CodeGen   : public ASTVisitor<CodeGen, llvm::Value *> { ... };
class ASTDumper : public ASTVisitor<ASTDumper> { ... };   // RetTy = void (기본값)
```

템플릿이라 인스턴스가 각각 따로 만들어지므로 가능합니다. 그래서 `CodeGen`의
노드 처리는 그냥 값을 반환합니다.

```cpp
Value *CodeGen::visitBinary(BinaryExprAST &E) {
  Value *L = visit(E.getLHS());      // 재귀도 그냥 반환값으로
  Value *R = visit(E.getRHS());
  if (!L || !R)
    return nullptr;
  ...
  return Builder->CreateFAdd(L, R, "addtmp");
}
```

**가상 함수였다면 이게 불가능합니다.** 가상 함수는 반환 타입을 방문자마다 다르게
할 수 없으니, `void visit(...)`로 고정하고 결과를 멤버 변수에 담아 두는 우회가
필요했습니다. 그 우회가 어떻게 생겼었는지도
[09. Visitor 설계 변천](09-visitor-evolution.md)에 있습니다.

### 4.5 방문자가 둘인 이유 — `ASTDumper`

`CodeGen` 말고 하나가 더 있습니다. `ASTDumper`는 트리를 들여쓰기해서 출력합니다.
`--dump-ast`가 이것입니다.

두 방문자를 비교하면 패턴의 값어치가 보입니다.

| | 만들어 내는 것 | `RetTy` | LLVM 헤더 |
| --- | --- | --- | --- |
| `CodeGen` | `llvm::Value*` | `llvm::Value *` | IR, 패스 등 다수 |
| `ASTDumper` | 화면 출력 | `void` (기본값) | **전혀 없음** |

`ASTDumper`는 `<ostream>` 하나만 씁니다. 같은 AST를 LLVM과 무관한 방식으로도
소비할 수 있다는 증거입니다.

원본 튜토리얼에서는 `dump()`가 `codegen()` 바로 옆에 가상 함수로 붙어 있어서
노드가 두 가지 책임을 동시에 졌습니다. 지금은 노드가 어느 쪽도 모릅니다.
세 번째 소비자(타입 검사기, 상수 폴딩 등)를 추가해도 **기존 노드는 한 줄도
건드릴 필요가 없습니다.**

```cpp
// src/ASTDumper.cpp — 반환할 것이 없으므로 void
void ASTDumper::visitBinary(BinaryExprAST &E) {
  line("Binary ") << '\'' << E.getOp() << "' @" << E.getLine() << ':'
                  << E.getCol() << '\n';
  child("LHS:", E.getLHS());
  child("RHS:", E.getRHS());
}
```

거꾸로, **노드 종류를 늘리는 것은 비싸집니다.** `ASTVisitor.h`의 스위치와
방문자 두 개를 모두 고쳐야 합니다. 이 맞교환이 무엇인지, 그리고 MLIR처럼
연산 집합이 열려 있는 IR에서는 왜 이 설계가 아예 성립하지 않는지는
[09. Visitor 설계 변천](09-visitor-evolution.md)에서 다룹니다.

---

## 5. LLVM 스타일 RTTI — `Kind` + `classof`

가끔은 "이 노드가 변수 참조인가?"를 직접 물어야 합니다. C++에는
`dynamic_cast`라는 기능이 있지만, **LLVM은 보통 이 기능을 끄고 빌드됩니다**
(`-fno-rtti`). 속도와 크기 때문입니다.

그래서 LLVM은 직접 만든 방식을 씁니다.

```cpp
class ExprAST {
public:
  enum ExprASTKind {
    Expr_Number, Expr_Variable, Expr_Unary, Expr_Binary,
    Expr_Assign, Expr_Call, Expr_If, Expr_For, Expr_Var,
  };
private:
  const ExprASTKind Kind;      // 자기가 무엇인지 스스로 기록
public:
  ExprASTKind getKind() const { return Kind; }
};
```

각 자식은 `classof`를 하나 제공합니다.

```cpp
static bool classof(const ExprAST *E) { return E->getKind() == Expr_Variable; }
```

이 두 조각만 있으면 LLVM의 캐스팅 도구가 동작합니다.

```cpp
isa<VariableExprAST>(E)        // 맞는 타입인가? (bool)
dyn_cast<VariableExprAST>(E)   // 맞으면 변환, 아니면 nullptr
cast<VariableExprAST>(E)       // 맞다고 확신할 때 (아니면 assert로 죽음)
```

`dynamic_cast`보다 훨씬 빠릅니다. 상속 계층을 훑는 대신 정수 비교 하나이기
때문입니다. MLIR의 Toy 튜토리얼도 같은 구조를 쓰므로, MLIR로 넘어갈 때 그대로
이어집니다.

이 때문에 `AST.h`가 LLVM 헤더 하나(`llvm/Support/Casting.h`)를 포함합니다.
다만 이것은 헤더만으로 동작하는 유틸리티이고, **IR 관련 의존성은 여전히
전혀 없습니다.**

Visitor가 있으니 실제로 이 캐스팅이 쓰이는 곳은 딱 한 군데,
`Parser::parseBinOpRHS`입니다 → [03-parser](03-parser.md) 6절

---

## 6. `AssignExprAST` — 원본과 다른 부분

원본 튜토리얼은 `x = 5`를 `BinaryExprAST`(연산자 `'='`)로 만들고,
코드 생성 단계에서 왼쪽이 변수인지 검사합니다.

```cpp
// 원본 튜토리얼 — BinaryExprAST::codegen 안
VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
if (!LHSE)
  return LogErrorV("destination of '=' must be a variable");
```

여기에 문제가 둘입니다.

1. `static_cast`는 **검사하지 않는** 변환입니다. 절대 `nullptr`을 돌려주지
   않으므로 `if (!LHSE)`는 **절대 참이 되지 않는 죽은 코드**입니다
2. 그래서 `2 = 3` 같은 입력이 오면, 숫자 노드를 변수 노드인 척 읽어서
   **실제로 세그멘테이션 폴트가 납니다** (직접 확인함, Ch7·Ch9 모두)

이 저장소는 "`=`의 왼쪽은 식별자여야 한다"가 **문법 규칙**이라는 점에 주목해서
파서에서 검사하고, 이름을 이미 담은 전용 노드를 만듭니다.

```cpp
class AssignExprAST : public ExprAST {
  std::string Name;                  // 이름을 문자열로 직접 보관
  std::unique_ptr<ExprAST> Value;
public:
  const std::string &getName() const { return Name; }
  ExprAST &getValue() const { return *Value; }
};
```

효과: CodeGen에는 **캐스팅도, 실패 경로도 없습니다.** "변수가 아닌 것에 대입"
이라는 상태 자체를 **표현할 수 없게** 만든 것입니다. 나중에 거부하는 것보다
강한 보장입니다.

```
$ echo '2 = 3;' | ./build/toy
Error: destination of '=' must be a variable      ← 파싱 단계에서

$ echo '2 = 3;' | ./ref7        # 원본 튜토리얼 Chapter7
Segmentation fault
```

---

## 7. 트리의 소유권 — `unique_ptr`

자식 노드는 `unique_ptr`로 들고 있습니다.

```cpp
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;
public:
  BinaryExprAST(SourceLocation Loc, char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Expr_Binary, Loc), Op(Op), LHS(std::move(LHS)),
        RHS(std::move(RHS)) {}
```

의미: **부모가 자식을 소유합니다.** 루트가 사라지면 트리 전체가 자동으로
해제됩니다. `delete`를 직접 쓸 일이 없습니다.

`std::move`가 필요한 이유는 `unique_ptr`이 복사되지 않기 때문입니다.
"주인이 둘"이 되면 두 번 해제되어 프로그램이 깨집니다. 그래서 소유권을
넘기는 `move`만 허용됩니다.

읽기용 접근자는 참조를 돌려줍니다.

```cpp
ExprAST &getLHS() const { return *LHS; }   // 소유권은 그대로, 보기만 함
```

`ForExprAST::getStep()`만 예외적으로 포인터입니다.

```cpp
ExprAST *getStep() const { return Step.get(); }   // 없을 수 있음(= 1.0)
```

`for i = 1, i < n in ...` 처럼 증가폭을 생략할 수 있어서, **없음**을 표현해야
하기 때문입니다. 참조는 "없음"을 표현할 수 없으므로 포인터를 씁니다.

### `FunctionAST::takeProto()`

```cpp
std::unique_ptr<PrototypeAST> takeProto() { return std::move(Proto); }
```

CodeGen이 프로토타입의 **소유권을 가져갑니다.** 나중에 새 모듈에 함수를 다시
선언할 때 필요하기 때문입니다 → [04-codegen](04-codegen.md) 5절

호출 후 `FunctionAST`는 더 이상 프로토타입을 갖고 있지 않으므로, **`takeProto()`
전에 `getProto()`로 참조를 확보해 둬야 합니다.** 원본 튜토리얼은 여기서
순서를 틀려서 버그가 있었고, 이 저장소는 고쳤습니다.

---

## 8. 익명 네임스페이스를 쓰지 않은 이유

원본 튜토리얼은 AST 클래스를 이름 없는 네임스페이스로 감쌉니다.

```cpp
// 원본 튜토리얼
namespace {
class ExprAST { ... };
...
} // end anonymous namespace
```

`.cpp` 파일 하나 안에서는 문제없습니다. "이 파일 전용"이라는 뜻이니까요.

그런데 **헤더에서는 재앙입니다.** 헤더는 여러 `.cpp`에 복사되어 들어가는데,
익명 네임스페이스는 각 파일마다 **서로 다른 타입**을 만들어 냅니다.

```
Parser.cpp 의 ExprAST   ≠   main.cpp 의 ExprAST     ← 이름은 같지만 다른 타입
```

`Parser.h`가 돌려주는 `unique_ptr<ExprAST>`를 `main.cpp`가 받을 수 없게 되고,
링크 에러 아니면 더 나쁘게는 조용한 오작동이 납니다.

그래서 파일을 나누는 순간 익명 네임스페이스는 **반드시** 제거해야 합니다.
튜토리얼의 모든 `static` 전역 변수도 같은 이유로 클래스 멤버가 됐습니다
(`static`은 "이 파일 전용"이라는 뜻이라 파일마다 사본이 생깁니다).

---

## 9. 정리

- AST는 우선순위와 괄호가 이미 반영된 **구조**다
- 이 저장소의 노드는 **데이터만** 갖는다. 튜토리얼의 전역 변수는 노드에
  `codegen()`을 달았기 때문에 생긴 것이다
- Visitor = CRTP + `Kind` 스위치. 분기는 `ASTVisitor.h` 한 곳에만 있고,
  노드를 추가하면 `-Wswitch`가 거기를 지목한다
- 방문자마다 반환 타입이 다르다 (`RetTy`). 가상 함수로는 불가능했던 부분
  — [09. Visitor 설계 변천](09-visitor-evolution.md)
- `Kind` + `classof` = LLVM 방식 RTTI. `dynamic_cast`보다 빠르고 `-fno-rtti`에서도 동작
- `AssignExprAST`로 "변수 아닌 것에 대입"을 표현 불가능하게 만들었다 (원본은 세그폴트)
- 헤더에서 익명 네임스페이스 금지

**다음**: [03. Parser](03-parser.md) — 토큰에서 이 트리를 만들어 내는 법
