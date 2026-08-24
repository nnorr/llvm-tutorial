# 09. Visitor 설계 변천 — 왜 세 번 바뀌었나

**파일**: `include/ASTVisitor.h`, `include/AST.h`
**관련 문서**: [02. AST](02-ast.md) 4절

이 저장소의 AST 순회 방식은 세 단계를 거쳤습니다. 지금 코드는 3판이지만,
1판과 2판이 왜 그렇게 생겼고 무엇 때문에 버렸는지를 알아야 3판이 무슨 문제를
푸는 것인지 보입니다. 각 단계는 **앞 단계의 구체적인 불편** 하나를 없앱니다.

이 문서는 그 비교를 남겨두기 위한 것입니다.

---

## 1. 세 판 한눈에

| | 분기 방식 | 상태 위치 | 반환 타입 | AST가 아는 것 |
| --- | --- | --- | --- | --- |
| **1판** 원본 튜토리얼 | 노드의 `virtual codegen()` | **전역 변수** | `Value*` | LLVM 전부 |
| **2판** `accept`/`visit` | vtable 2회 (이중 디스패치) | 방문자 멤버 | `void` + `Result` 멤버 | 방문자의 존재 |
| **3판** 현재 | `Kind` 스위치 + CRTP | 방문자 멤버 | `RetTy` (자유) | **아무것도** |

---

## 2. 1판 — 노드가 스스로 코드를 생성한다

```cpp
// 원본 튜토리얼 (reference/kaleidoscope/Chapter9/toy.cpp)
class NumberExprAST : public ExprAST {
  double Val;
public:
  Value *codegen() override;
  raw_ostream &dump(raw_ostream &out, int ind) override;
};
```

노드가 IR을 직접 만듭니다. 그러려면 `IRBuilder`와 심볼 테이블이 필요한데
`codegen()`은 인자를 받지 않으므로, 그것들이 전부 전역이 됩니다.

```cpp
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, AllocaInst *> NamedValues;
```

### 무엇이 문제였나

**(a) 전역이라 인스턴스를 둘 만들 수 없습니다.** JIT은 `TheJIT->getDataLayout()`으로,
오브젝트 출력은 `TargetMachine::createDataLayout()`으로 모듈을 초기화해야 하는데,
`TheModule`이 하나뿐이라 두 경로가 공존할 수 없습니다. 그래서 원본은 Ch8에서
**JIT을 통째로 삭제하고** 오브젝트 출력으로 갈아탑니다. 우리 저장소가 REPL과
`-c`를 한 바이너리에 담을 수 있는 것이 이 문제를 없앤 결과입니다.

**(b) 연산을 추가하려면 노드를 전부 열어야 합니다.** Ch9가 `dump()`를 넣을 때
노드 클래스 9개를 모두 수정합니다.

**(c) AST가 LLVM에 묶입니다.** `Value *codegen()`이라는 시그니처 하나 때문에
AST 정의는 `IRBuilder`, `DIBuilder`, `Module`을 아는 파일 안에 있어야 합니다.

---

## 3. 2판 — `accept`/`visit` 이중 디스패치

교과서적인 Visitor 패턴입니다.

```cpp
// ASTVisitor.h (2판)
class ASTVisitor {
public:
  virtual void visit(NumberExprAST &E) = 0;
  virtual void visit(BinaryExprAST &E) = 0;
  // ... 9개
};

// AST.h (2판) — 노드마다 한 줄
void accept(ASTVisitor &V) override { V.visit(*this); }
```

`E.accept(CG)`를 부르면 vtable을 두 번 탑니다. 먼저 `accept`가 노드의 실제
타입으로 갈라지고, 그 안의 `V.visit(*this)`에서 `*this`의 정적 타입이
`NumberExprAST&`로 확정되어 오버로드가 골라진 뒤 방문자의 vtable로 갈라집니다.
그래서 **이중 디스패치(double dispatch)** 입니다.

### 무엇을 고쳤나

1판의 (a) (b) (c)를 전부 고쳤습니다. codegen 로직이 `CodeGen` 클래스로 나가면서
상태가 그 클래스의 멤버가 되었고(전역 소멸), `ASTDumper`가 `AST.h`를 한 줄도
건드리지 않고 추가되었고, `AST.h`의 LLVM 의존이 `llvm/Support/Casting.h` 하나로
줄었습니다.

> 여기서 (a)와 나머지를 구분해 둘 필요가 있습니다. **전역 제거는 Visitor의
> 결과이지 Visitor 그 자체가 아닙니다.** 노드에 메서드를 남긴 채 상태만 인자로
> 넘겨도 (a)는 해결됩니다.
>
> ```cpp
> virtual Value *codegen(CodeGen &CG) = 0;   // Visitor 아님, 전역은 없음
> ```
>
> 이러면 `-c`와 JIT 공존까지는 됩니다. 하지만 (b)(c)는 그대로 남습니다.
> 반환 타입이 `Value*`인 이상 AST는 LLVM을 알아야 하고, 새 연산은 여전히
> 노드 9개를 열어야 하니까요.

### 무엇이 남았나 — `Result` 멤버

`visit()`이 값을 반환할 수 없습니다. 반환하려면 `ASTVisitor.h`가 `llvm::Value`를
알아야 하고, `AST.h`가 `ASTVisitor.h`를 include하므로 (c)가 되살아납니다.

그래서 결과를 멤버에 담아 두고 나중에 꺼내는 우회가 필요했습니다.

```cpp
// CodeGen.h (2판)
llvm::Value *Result = nullptr;

// CodeGen.cpp (2판)
llvm::Value *CodeGen::codegenExpr(ExprAST &E) {
  Result = nullptr;        // 이전 노드 값이 새지 않도록
  E.accept(*this);
  return Result;
}
```

노드 처리는 이렇게 생겼습니다.

```cpp
// 2판
void CodeGen::visit(BinaryExprAST &E) {
  Value *L = codegenExpr(E.getLHS());
  Value *R = codegenExpr(E.getRHS());
  if (!L || !R) {
    Result = nullptr;      // 실패 경로마다 이 두 줄
    return;
  }
  switch (E.getOp()) {
  case '+':
    Result = Builder->CreateFAdd(L, R, "addtmp");
    return;
  ...
```

`CodeGen.cpp` 한 파일에 `Result = ...`가 **33곳** 있었습니다. `codegenExpr`이
매번 `Result`를 지웠으므로 "앞 노드의 값을 잘못 읽는" 버그는 막혀 있었지만,
반환 타입이 있었으면 컴파일러가 강제했을 것을 사람이 규율로 지키는 구조였습니다.

**이건 게으름이 아니라 `accept()`가 가상 함수라는 사실의 직접적인 귀결입니다.**
가상 함수는 반환 타입을 방문자마다 다르게 할 수 없습니다. 선택지는 둘뿐입니다.

1. `accept()` 유지 → 반환값은 멤버에 담는다
2. `accept()` 폐기, `Kind`로 스위치 → 반환 타입 자유

중간이 없습니다.

---

## 4. 3판 — `Kind` 스위치 + CRTP (현재)

Clang이 `StmtVisitor`에서 쓰는 방식입니다. 2번 선택지를 택했습니다.

```cpp
template <typename Derived, typename RetTy = void> class ASTVisitor {
  Derived &derived() { return *static_cast<Derived *>(this); }

public:
  RetTy visit(ExprAST &E) {
    switch (E.getKind()) {
    case ExprAST::Expr_Number:
      return derived().visitNumber(llvm::cast<NumberExprAST>(E));
    // ... 9개
    }
    llvm_unreachable("unknown ExprASTKind");
  }
};
```

재료는 이미 있었습니다. LLVM 스타일 RTTI를 위해 `ExprASTKind` + `classof`를
갖고 있었으므로([02. AST](02-ast.md) 5절), 스위치를 쓰는 데 새로 추가할 것이
없었습니다.

### 무엇이 좋아졌나

**(1) 반환 타입이 돌아왔습니다.**

```cpp
// 3판 — 위 2판 코드와 비교
Value *CodeGen::visitBinary(BinaryExprAST &E) {
  Value *L = visit(E.getLHS());
  Value *R = visit(E.getRHS());
  if (!L || !R)
    return nullptr;
  switch (E.getOp()) {
  case '+':
    return Builder->CreateFAdd(L, R, "addtmp");
  ...
```

`Result` 멤버, `codegenExpr` 우회, 실패 경로의 `Result = nullptr; return;` 짝이
전부 사라졌습니다. `CodeGen.cpp`가 **196줄 수정, 순 -41줄**입니다.

**(2) `AST.h`가 방문자를 모르게 되었습니다.** `accept()` 10줄과
`#include "ASTVisitor.h"`가 사라지고, 의존 방향이 뒤집혔습니다.

```
2판:  AST.h  ──include──▶  ASTVisitor.h
3판:  ASTVisitor.h  ──include──▶  AST.h
```

이제 노드에는 소비자를 위해 붙은 멤버가 하나도 없습니다.

**(3) vtable을 타지 않습니다.** `derived().visitNumber(...)`는 평범한 함수
호출이라 인라인될 수 있습니다. 다만 이건 부수 효과일 뿐, 이 저장소 규모에서
성능 차이는 의미가 없습니다.

### 무엇을 잃었나

**노드 추가 시 컴파일 에러 대신 경고입니다.** 2판은 순수 가상 함수라
방문자가 하나라도 빠뜨리면 컴파일이 실패했습니다. 3판은 `ASTVisitor.h`의
스위치에 `default:`가 없어서 `-Wswitch`가 뜨고, 그 케이스를 추가하면
`derived().visitXxx()`가 없다는 에러로 이어집니다. 결국 잡히긴 하지만
한 단계 늦습니다.

**CRTP는 다형적으로 담을 수 없습니다.** `ASTVisitor`가 템플릿이라
`ASTVisitor&`로 방문자를 받는 함수를 쓸 수 없습니다. 이 저장소는 `main.cpp`가
`CodeGen`과 `ASTDumper`를 구체 타입으로 쓰므로 문제가 없었지만, 방문자를
런타임에 골라야 하는 설계라면 3판이 오히려 불리합니다.

**교과서적인 모양에서 멀어졌습니다.** "Visitor 패턴"을 검색하면 나오는 것은
2판입니다. 학습 목적이라면 이건 실제 비용입니다.

### 검증

동작이 바뀌지 않았음을 이렇게 확인했습니다.

```bash
# 변환 전 기준선
./build/toy --dump-ast < tests/operators.ks > base.txt 2>&1
./build/toy -c tests/fib.ks -o base.o

# 변환 후 — AST 덤프, IR 출력, 오브젝트 파일 전부 동일
diff base.txt new.txt && md5sum base.o new.o
```

`--dump-ast` 출력, JIT이 찍는 IR, `-c`가 만든 오브젝트 파일(md5 동일),
`lexer_tests` 모두 변환 전과 일치합니다.

---

## 5. 그래도 풀리지 않는 것 — 닫힌 세계

여기까지가 이 저장소의 이야기고, 마지막으로 **세 판 모두가 공유하는 한계**가
있습니다.

셋 다 **노드 종류의 집합을 컴파일 타임에 안다**고 전제합니다. 3판으로 바꿔도
노드를 하나 추가하려면 `ExprASTKind`에 항목을 넣고, `ASTVisitor.h`의 스위치를
고치고, 방문자를 전부 수정해야 합니다. 2판보다 나아진 게 아니라 실패가
드러나는 방식만 다릅니다(에러 → 경고).

Kaleidoscope에서는 이게 문제가 아닙니다. 문법이 Ch7에서 끝났고 노드는 9개에서
늘지 않으니, **비용을 치를 축을 이 프로젝트는 밟지 않습니다.** 이득이 있는
"새 연산 추가" 축은 이미 두 번 썼고요(`CodeGen`, `ASTDumper`).

이건 Expression Problem이라고 부르는 맞교환입니다.

| | 새 **연산** 추가 | 새 **노드 타입** 추가 |
| --- | --- | --- |
| 1판 (노드에 메서드) | 노드 9개 전부 수정 | 클래스 하나 작성 |
| 2·3판 (visitor) | 클래스 하나 작성 | 스위치 + 방문자 전부 수정 |

어느 쪽도 둘 다 싸게 만들지 못합니다. **연산 축을 싸게 만드는 대신 노드 축을
비싸게 만든 것**이고, 문법이 동결된 언어에서는 옳은 거래입니다.

### MLIR은 왜 다른가

문법이 동결되지 않는 경우가 실제로 있습니다. MLIR은 dialect가 연산을
**런타임에 등록**하므로, 연산의 집합을 컴파일 타임에 알 수 없습니다.
위 표의 어느 칸도 성립하지 않습니다.

그래서 MLIR은 축을 바꿉니다.

- 연산이 C++ 클래스 계층이 아닙니다. `Operation*` 하나에 동적인
  `OperationName`이 붙습니다. 열거할 `Kind` enum이 없습니다.
- 디스패치가 **인터페이스 질의**입니다. 패스가 "이 op은 어떤 종류인가"를
  묻는 대신, op이 "나는 이 `OpInterface`를 구현한다"고 선언하고 패스는
  인터페이스로 질의합니다. 등록 방향이 뒤집혀 있습니다.
- 변환은 op 이름으로 매칭되는 `RewritePattern`이 자기를 등록하는 방식입니다.
  패스가 op 목록을 나열하지 않습니다.

**즉 3판으로의 변환은 MLIR과 무관합니다.** Clang식 `Kind` 스위치도 닫힌
세계이므로 dialect 문제를 풀지 못합니다. 3판을 택한 이유는 어디까지나
`Result` 제거와 `AST.h` 분리이고, 열린 확장성이 필요하면 그건 visitor를
개선하는 문제가 아니라 **IR 표현 자체를 다시 설계하는 문제**입니다.

MLIR의 Toy 튜토리얼이 AST 단계에서는 우리와 같은 `Kind` + `classof` 모양을
쓰다가, MLIR로 넘어가는 순간 그 구조를 통째로 버리는 것이 그 얘기입니다.

---

## 6. 정리

- 1판 → 2판: 전역이 사라지고 `-c`와 JIT이 공존 가능해졌다. 다만 **전역 제거는
  Visitor의 결과이지 Visitor의 정의가 아니다** — 노드 메서드에 상태를 인자로
  넘기기만 해도 됐다
- 2판 → 3판: `accept()`가 가상 함수라서 강요됐던 `Result` 멤버가 사라졌다.
  `AST.h`가 방문자의 존재조차 모르게 됐다
- 대가: 노드 추가가 컴파일 에러 대신 `-Wswitch` 경고로 잡힌다. 방문자를
  다형적으로 담을 수 없다. 교과서적인 모양에서 멀어졌다
- 세 판 모두 닫힌 세계다. MLIR의 dialect 확장성은 이 축에서 얻어지지 않는다

**돌아가기**: [02. AST](02-ast.md) — 지금 코드의 설명
