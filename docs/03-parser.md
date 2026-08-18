# 03. Parser — 토큰에서 트리 만들기

**파일**: `include/Parser.h`, `src/Parser.cpp`, `include/OperatorTable.h`
**원본 튜토리얼**: Chapter 2 (기본), Chapter 5 (제어 흐름), Chapter 6 (사용자 정의 연산자), Chapter 7 (변수)

---

## 1. 재귀 하강 파싱

Kaleidoscope 파서는 **재귀 하강(recursive descent)** 방식입니다. 원리는 단순합니다.
문법 규칙 하나당 함수 하나를 만들고, 규칙이 서로를 참조하면 함수도 서로를 호출합니다.

```
문법                                    함수
────────────────────────────────────────────────────────────
expression ::= unary binoprhs           parseExpression()
unary      ::= primary | '!' unary      parseUnary()
primary    ::= 식별자 | 숫자 | '(' … )'  parsePrimary()
                | ifexpr | forexpr | varexpr
```

`parseExpression`이 `parseUnary`를 부르고, `parseUnary`가 `parsePrimary`를 부르고,
`parsePrimary`가 괄호를 만나면 다시 `parseExpression`을 부릅니다. 이 **재귀**가
중첩된 식을 자연스럽게 처리합니다.

`src/Parser.cpp`의 함수 이름을 위 문법과 나란히 놓고 읽으면 대응이 그대로 보입니다.

---

## 2. 클래스 구조 — 15개 함수, 공개는 6개

```cpp
class Parser {
  Lexer &Lex;                     // 토큰 공급자 (빌려 씀)
  OperatorTable &Ops;             // 연산자 우선순위표 (공유)

  int CurTok = 0;                 // 지금 보고 있는 토큰

  int getNextToken() { return CurTok = Lex.gettok(); }
  int getTokPrecedence() const { return Ops.getPrecedence(CurTok); }

  // 여기부터 전부 private — 내부 구현
  std::unique_ptr<ExprAST> parseExpression();
  std::unique_ptr<ExprAST> parseNumberExpr();
  std::unique_ptr<ExprAST> parseParenExpr();
  std::unique_ptr<ExprAST> parseIdentifierExpr();
  std::unique_ptr<ExprAST> parseIfExpr();
  std::unique_ptr<ExprAST> parseForExpr();
  std::unique_ptr<ExprAST> parseVarExpr();
  std::unique_ptr<ExprAST> parsePrimary();
  std::unique_ptr<ExprAST> parseUnary();
  std::unique_ptr<ExprAST> parseBinOpRHS(int ExprPrec,
                                         std::unique_ptr<ExprAST> LHS);
  std::unique_ptr<PrototypeAST> parsePrototype();

public:
  Parser(Lexer &Lex, OperatorTable &Ops);

  int getCurTok() const { return CurTok; }
  void advance() { getNextToken(); }

  std::unique_ptr<FunctionAST>  parseDefinition();
  std::unique_ptr<PrototypeAST> parseExtern();
  std::unique_ptr<FunctionAST>  parseTopLevelExpr(
      const std::string &Name = "__anon_expr");
};
```

원본 튜토리얼에서는 이 15개가 전부 파일 전역 함수라 서로 구분이 없었습니다.
클래스로 묶으면 **바깥에서 실제로 필요한 것이 6개뿐**임이 드러납니다.
드라이버(`main.cpp`)가 부르는 것은 정확히 이 6개입니다.

### 2.1 `CurTok` — 1토큰 미리보기

파서는 "지금 보고 있는 토큰" 하나를 항상 들고 있습니다.

```cpp
int getNextToken() { return CurTok = Lex.gettok(); }
```

`CurTok`을 보고 무엇을 파싱할지 정한 뒤, 소비했으면 `getNextToken()`으로
다음 것을 당겨옵니다.

> **왜 Lexer가 아니라 Parser에 있는가**
> 미리보기 버퍼는 "파싱하기 위한 편의"이지 "토큰을 만드는 일"이 아닙니다.
> Lexer는 순수한 토큰 공급자로 남기고, 버퍼는 소비자인 Parser가 갖습니다.
> Lexer에 넣으면 나중에 미리보기를 2개로 늘리고 싶을 때 Lexer를 고쳐야 합니다.

### 2.2 생성자가 첫 토큰을 당긴다

```cpp
Parser::Parser(Lexer &Lex, OperatorTable &Ops) : Lex(Lex), Ops(Ops) {
  getNextToken();     // CurTok을 유효한 상태로 만들어 둠
}
```

생성 직후부터 `getCurTok()`이 의미 있는 값을 돌려줍니다. 원본 튜토리얼은
`main()`에서 수동으로 `getNextToken()`을 한 번 부르는데, 잊으면 오동작합니다.
생성자에 넣어 그 실수를 없앴습니다.

### 2.3 에러 처리

```cpp
namespace {
std::unique_ptr<ExprAST> logError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}
std::unique_ptr<PrototypeAST> logErrorP(const char *Str) {
  logError(Str);
  return nullptr;
}
} // namespace
```

에러 메시지를 찍고 **`nullptr`을 반환**합니다. 그래서 파싱 함수의 규칙은
"성공하면 노드, 실패하면 `nullptr`"입니다. 호출한 쪽은 매번 확인합니다.

```cpp
auto V = parseExpression();
if (!V)
  return nullptr;      // 실패를 위로 전파
```

`logError`와 `logErrorP`가 나뉜 이유는 **반환 타입이 달라야 하기** 때문입니다.
C++에서는 `return logError(...)` 한 줄로 쓰려면 타입이 맞아야 합니다.

이름 없는 `namespace { }` 로 감싼 것은 "이 파일 안에서만 쓴다"는 뜻입니다.
헤더가 아니라 `.cpp`이므로 안전합니다.

---

## 3. 단순한 파싱 함수들

### 3.1 숫자

```cpp
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
  SourceLocation Loc = Lex.getCurLoc();
  auto Result = std::make_unique<NumberExprAST>(Loc, Lex.getNumVal());
  getNextToken();      // 숫자 토큰 소비
  return Result;
}
```

`std::make_unique<T>(...)`는 `T` 객체를 새로 만들어 `unique_ptr`에 담아 줍니다.

**위치를 먼저 저장하는 것이 중요합니다.** `getNextToken()` 후에는 Lexer의
위치가 다음 토큰으로 넘어가 버립니다.

`auto`는 "타입을 컴파일러가 알아서 추론"입니다. 여기서는
`std::unique_ptr<NumberExprAST>`가 됩니다.

### 3.2 괄호

```cpp
std::unique_ptr<ExprAST> Parser::parseParenExpr() {
  getNextToken();                  // '(' 소비
  auto V = parseExpression();      // 재귀
  if (!V)
    return nullptr;

  if (CurTok != ')')
    return logError("expected ')'");
  getNextToken();                  // ')' 소비
  return V;
}
```

**괄호에 해당하는 AST 노드는 없습니다.** 괄호는 파싱 순서만 바꾸고, 그 결과가
트리 모양에 이미 반영되므로 따로 저장할 필요가 없습니다.

### 3.3 식별자와 함수 호출

```cpp
std::unique_ptr<ExprAST> Parser::parseIdentifierExpr() {
  std::string IdName = Lex.getIdentifierStr();
  SourceLocation LitLoc = Lex.getCurLoc();

  getNextToken();                  // 식별자 소비

  if (CurTok != '(')               // 뒤에 '('가 없으면 그냥 변수
    return std::make_unique<VariableExprAST>(LitLoc, IdName);

  // '('가 있으면 함수 호출
  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = parseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;

      if (CurTok != ',')
        return logError("Expected ')' or ',' in argument list");
      getNextToken();
    }
  }
  getNextToken();                  // ')' 소비

  return std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
}
```

`x`인지 `x(1,2)`인지는 **다음 토큰을 봐야** 알 수 있습니다. 이것이 1토큰
미리보기가 필요한 이유입니다.

`Args.push_back(std::move(Arg))`에서 `move`가 필요한 이유: `unique_ptr`은
복사할 수 없고, 벡터 안으로 소유권을 넘겨야 하기 때문입니다.

### 3.4 `if` / `for` / `var`

구조는 모두 비슷합니다. 키워드를 소비하고, 정해진 순서대로 부분들을 파싱하고,
중간에 기대한 토큰이 없으면 에러를 냅니다.

```cpp
std::unique_ptr<ExprAST> Parser::parseIfExpr() {
  SourceLocation IfLoc = Lex.getCurLoc();
  getNextToken();                       // 'if' 소비

  auto Cond = parseExpression();
  if (!Cond) return nullptr;

  if (CurTok != tok_then)
    return logError("expected then");
  getNextToken();

  auto Then = parseExpression();
  if (!Then) return nullptr;

  if (CurTok != tok_else)
    return logError("expected else");
  getNextToken();

  auto Else = parseExpression();
  if (!Else) return nullptr;

  return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then),
                                     std::move(Else));
}
```

`else`가 **필수**임에 주목하세요. `if`가 식이라서 반드시 값을 내야 하는데,
`else`가 없으면 조건이 거짓일 때 값이 없어집니다.

`parseForExpr`에서 증가폭만 선택적입니다.

```cpp
std::unique_ptr<ExprAST> Step;      // 비어 있을 수 있음
if (CurTok == ',') {
  getNextToken();
  Step = parseExpression();
  if (!Step) return nullptr;
}
```

`parseVarExpr`은 `var a = 1, b, c = 3 in ...` 처럼 여러 변수를 받으므로
`(이름, 초기식)` 쌍의 벡터를 만듭니다. 초기식은 생략 가능해서 `nullptr`일 수
있고, CodeGen이 그 경우 `0.0`을 넣습니다.

---

## 4. 연산자 우선순위 — `OperatorTable`

```cpp
class OperatorTable {
  std::map<char, int> Precedence;

public:
  OperatorTable() {
    Precedence['='] = 2;
    Precedence['<'] = 10;
    Precedence['+'] = 20;
    Precedence['-'] = 20;
    Precedence['*'] = 40;   // 가장 높음
  }

  int getPrecedence(int Tok) const {
    if (!isascii(Tok))
      return -1;
    auto It = Precedence.find(static_cast<char>(Tok));
    if (It == Precedence.end() || It->second <= 0)
      return -1;
    return It->second;
  }

  void setPrecedence(char Op, int Prec) { Precedence[Op] = Prec; }
  void erase(char Op) { Precedence.erase(Op); }
};
```

숫자가 클수록 강하게 결합합니다. `*`(40)이 `+`(20)보다 크므로 `1+2*3`은
`1+(2*3)`이 됩니다.

**`-1`은 "이항 연산자가 아님"** 을 뜻합니다. `tok_identifier` 같은 음수 토큰이나
등록되지 않은 문자가 오면 `-1`이 나오고, 그러면 파서는 "식이 여기서 끝났다"고
판단합니다.

### 왜 Parser도 CodeGen도 아닌 별도 클래스인가

사용자가 연산자를 직접 정의할 수 있습니다.

```
def binary | 5 (LHS RHS) ...
```

이때 **우선순위를 등록하는 시점이 코드 생성 시점**입니다 (`CodeGen`이
`Ops.setPrecedence(...)`를 호출). 그런데 **읽는 쪽은 Parser**입니다.

```
Parser  ──읽기──▶  OperatorTable  ◀──쓰기──  CodeGen
```

이 표를 Parser가 소유하면 CodeGen이 Parser에 의존하고, CodeGen이 소유하면
그 반대가 되어 **순환 의존**이 생깁니다. 그래서 어느 쪽도 소유하지 않고
`main`이 만들어 둘 다에게 참조로 건넵니다.

전역 변수 하나였을 때는 이 관계가 보이지 않던 것이, 파일을 나누자 드러난
사례입니다.

---

## 5. `parseBinOpRHS` — 우선순위 등반

파서에서 가장 어려운 함수입니다. 천천히 봅시다.

```cpp
std::unique_ptr<ExprAST> Parser::parseExpression() {
  auto LHS = parseUnary();
  if (!LHS)
    return nullptr;
  return parseBinOpRHS(0, std::move(LHS));
}
```

먼저 왼쪽 피연산자 하나를 읽고, 나머지를 `parseBinOpRHS`에 맡깁니다.

```cpp
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int ExprPrec,
                                               std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = getTokPrecedence();

    // (1) 지금 연산자가 너무 약하면 여기서 멈추고 LHS를 돌려준다
    if (TokPrec < ExprPrec)
      return LHS;

    int BinOp = CurTok;
    SourceLocation BinLoc = Lex.getCurLoc();
    getNextToken();                     // 연산자 소비

    // (2) 오른쪽 피연산자 하나를 읽는다
    auto RHS = parseUnary();
    if (!RHS)
      return nullptr;

    // (3) 다음 연산자가 더 강하면, RHS를 그쪽에 먼저 넘긴다
    int NextPrec = getTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = parseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // (4) 합쳐서 새로운 LHS로
    LHS = std::make_unique<BinaryExprAST>(BinLoc, static_cast<char>(BinOp),
                                          std::move(LHS), std::move(RHS));
  }
}
```

`ExprPrec`은 **"이 호출은 이 세기 이상의 연산자만 처리한다"** 는 하한선입니다.

### 5.1 예제 1 — `1 + 2 + 3` (같은 우선순위)

`parseExpression`: `LHS = 1`, `parseBinOpRHS(0, 1)` 호출

| 단계 | 상태 |
| --- | --- |
| 반복 1 | `+`(20) ≥ 0 → 진행. `RHS = 2`. 다음 연산자 `+`(20)은 20보다 크지 않음 → 그냥 합침. `LHS = (1+2)` |
| 반복 2 | `+`(20) ≥ 0 → 진행. `RHS = 3`. 다음은 `;`(-1) → 합침. `LHS = ((1+2)+3)` |
| 반복 3 | `;` → `-1 < 0` → 종료 |

결과: `((1+2)+3)` — **왼쪽 결합**입니다.

### 5.2 예제 2 — `1 + 2 * 3` (오른쪽이 더 강함)

`parseBinOpRHS(0, 1)`

| 단계 | 상태 |
| --- | --- |
| 반복 1 | `+`(20) ≥ 0 → 진행. `RHS = 2` |
| | 다음 연산자 `*`는 40. **20 < 40** → (3)번 분기 발동 |
| | `parseBinOpRHS(21, 2)` 재귀 호출 ← 하한선을 21로 올림 |
| 재귀 | `*`(40) ≥ 21 → 진행. `RHS = 3`. 다음은 `;`(-1) → 합쳐서 `(2*3)` 반환 |
| 반복 1 계속 | `RHS = (2*3)`. 합쳐서 `LHS = (1+(2*3))` |
| 반복 2 | `;` → 종료 |

결과: `(1+(2*3))` ✅

**`TokPrec + 1`의 의미**: "나(`+`, 20)보다 **엄격히 강한** 연산자만 먼저
가져가라". 만약 `TokPrec`(20)을 그대로 넘기면 같은 세기의 `+`도 재귀 안에서
처리되어 오른쪽 결합이 되어 버립니다. `+1`이 왼쪽 결합을 만듭니다.

### 5.3 `parseUnary`가 그 사이에 있는 이유

```cpp
std::unique_ptr<ExprAST> Parser::parseUnary() {
  if (!isascii(CurTok) || CurTok == '(' || CurTok == ',')
    return parsePrimary();

  SourceLocation OpLoc = Lex.getCurLoc();
  int Opc = CurTok;
  getNextToken();
  if (auto Operand = parseUnary())      // 자기 자신을 재귀 호출
    return std::make_unique<UnaryExprAST>(OpLoc, static_cast<char>(Opc),
                                          std::move(Operand));
  return nullptr;
}
```

`parseBinOpRHS`가 피연산자를 읽을 때 `parsePrimary`가 아니라 `parseUnary`를
부릅니다. 그래서 **단항 연산자는 항상 이항 연산자보다 강하게 결합합니다.**

`!a + b` → `(!a) + b`. 단항에는 우선순위 표가 아예 없습니다. 문법상 위치가
고정되어 있어서 필요가 없기 때문입니다.

자기 자신을 재귀 호출하므로 `!!7`, `- - 4` 처럼 연달아 쓸 수 있습니다.

첫 줄의 조건이 하는 일: 지금 토큰이 음수(= `tok_number` 같은 진짜 토큰)이거나
`(`, `,` 이면 연산자가 아니므로 `parsePrimary`로 보냅니다.

---

## 6. `=` 처리 — 원본과 다른 부분

`parseBinOpRHS`의 (4)번 합치는 자리에 분기가 있습니다.

```cpp
if (BinOp == '=') {
  auto *LHSVar = llvm::dyn_cast<VariableExprAST>(LHS.get());
  if (!LHSVar)
    return logError("destination of '=' must be a variable");

  LHS = std::make_unique<AssignExprAST>(BinLoc, LHSVar->getName(),
                                        std::move(RHS));
} else {
  LHS = std::make_unique<BinaryExprAST>(BinLoc, static_cast<char>(BinOp),
                                        std::move(LHS), std::move(RHS));
}
```

`dyn_cast`는 [02-ast](02-ast.md) 5절의 `Kind`/`classof` 위에서 동작합니다.
맞으면 변환된 포인터, 아니면 `nullptr`입니다.

**여기서 걸러내면 CodeGen에는 검사 자체가 필요 없어집니다.** `AssignExprAST`는
이름을 문자열로 이미 갖고 있으니까요. 원본 튜토리얼은 이 검사를 CodeGen에서
`static_cast`로 하는데, 그 검사는 동작하지 않아 `2 = 3` 입력에 세그폴트가 납니다.

---

## 7. `parsePrototype` — 함수 시그니처와 연산자 정의

세 가지 형태를 한 함수가 처리합니다.

```
prototype ::= id '(' id* ')'                  일반 함수
            | 'binary' LETTER number? '(' id id ')'
            | 'unary'  LETTER '(' id ')'
```

```cpp
std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
  std::string FnName;
  SourceLocation FnLoc = Lex.getCurLoc();

  unsigned Kind = 0;              // 0=일반, 1=단항, 2=이항
  unsigned BinaryPrecedence = 30; // 기본 우선순위

  switch (CurTok) {
  default:
    return logErrorP("Expected function name in prototype");

  case tok_identifier:
    FnName = Lex.getIdentifierStr();
    Kind = 0;
    getNextToken();
    break;

  case tok_unary:
    getNextToken();
    if (!isascii(CurTok))
      return logErrorP("Expected unary operator");
    FnName = "unary";
    FnName += static_cast<char>(CurTok);    // "unary" + '!' → "unary!"
    Kind = 1;
    getNextToken();
    break;

  case tok_binary:
    getNextToken();
    if (!isascii(CurTok))
      return logErrorP("Expected binary operator");
    FnName = "binary";
    FnName += static_cast<char>(CurTok);    // "binary" + '|' → "binary|"
    Kind = 2;
    getNextToken();

    if (CurTok == tok_number) {             // 우선순위 지정이 있으면
      double Val = Lex.getNumVal();
      if (Val < 1 || Val > 100)
        return logErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = static_cast<unsigned>(Val);
      getNextToken();
    }
    break;
  }
  ...
```

**핵심**: 사용자 정의 연산자는 **이름이 조작된 평범한 함수**입니다.
`def binary | 5 (LHS RHS) ...` 는 `binary|` 라는 이름의 함수를 선언합니다.
전용 타입도, 전용 처리 경로도 없습니다.

실제 오브젝트 파일에서 확인할 수 있습니다.

```bash
$ ./build/toy -c tests/fib.ks -o /tmp/f.o && nm /tmp/f.o | grep ' T '
0000000000000000 T binary:      ← def binary : 1 (x y) y;
0000000000000010 T fib
0000000000000090 T main
```

인자 개수 검사도 `Kind`를 재활용합니다.

```cpp
if (Kind && ArgNames.size() != Kind)
  return logErrorP("Invalid number of operands for operator");
```

`Kind`가 필요한 인자 개수와 같은 값(단항 1, 이항 2)이라 그대로 비교하면 됩니다.
일반 함수는 `Kind == 0`이라 `if`가 통째로 건너뛰어집니다.

---

## 8. 최상위 진입점 3개

```cpp
// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> Parser::parseDefinition() {
  getNextToken();                      // 'def' 소비
  auto Proto = parsePrototype();
  if (!Proto) return nullptr;

  if (auto E = parseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> Parser::parseExtern() {
  getNextToken();                      // 'extern' 소비
  return parsePrototype();
}

// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> Parser::parseTopLevelExpr(const std::string &Name) {
  SourceLocation FnLoc = Lex.getCurLoc();
  if (auto E = parseExpression()) {
    auto Proto = std::make_unique<PrototypeAST>(FnLoc, Name,
                                                std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}
```

`parseTopLevelExpr`이 흥미롭습니다. `1+2;` 처럼 그냥 식을 입력해도, 인자가
없는 **함수로 감싸서** 반환합니다. 그래야 CodeGen이 함수 하나를 만들고 JIT이
그것을 호출할 수 있습니다.

이름이 매개변수인 이유는 모드마다 달라야 하기 때문입니다.

| 모드 | 이름 | 이유 |
| --- | --- | --- |
| JIT (기본) | `__anon_expr` | 만들고 호출한 뒤 버림 |
| `-c` (컴파일) | `main` | 오브젝트 파일의 진입점 |

컴파일 모드에서 최상위 식을 **하나만** 허용하는 이유가 여기 있습니다.
두 번째가 오면 `main`을 다시 정의하게 됩니다. 원본 튜토리얼 Ch8이 언급하는
바로 그 제약입니다. (기본값 `= "__anon_expr"` 은 **기본 인자**로, 인자를
생략하면 이 값이 쓰입니다.)

---

## 9. 직접 확인해 보기

```bash
# 우선순위가 트리에 어떻게 반영되는지
echo '1 + 2 * 3;'   | ./build/toy --dump-ast
echo '(1 + 2) * 3;' | ./build/toy --dump-ast

# 단항이 이항보다 강한지
echo 'def unary!(v) if v then 0 else 1;
      !1 + 1;'      | ./build/toy --dump-ast

# 사용자 정의 연산자
echo 'def binary $ 40 (a b) a - b;
      10 $ 3 $ 2;'  | ./build/toy --dump-ast

# 파싱 단계 에러
echo '2 = 3;'       | ./build/toy
echo '(1 + 2;'      | ./build/toy
```

---

## 10. 정리

- 문법 규칙 하나 = 함수 하나. 규칙의 재귀가 함수의 재귀가 된다
- `CurTok` 1토큰 미리보기로 무엇을 파싱할지 결정한다. 버퍼는 Lexer가 아닌 Parser 소유
- `parseBinOpRHS`의 `ExprPrec`은 하한선. `TokPrec + 1`이 왼쪽 결합을 만든다
- 단항 연산자는 우선순위가 없다. `parseUnary`가 피연산자 자리에 있어 항상 더 강하다
- `OperatorTable`은 Parser도 CodeGen도 소유하지 않는다 — 순환 의존을 끊기 위해
- 사용자 정의 연산자 = 이름이 `binary|`, `unary!` 인 평범한 함수
- 파서가 만든 노드는 **소유권과 함께** 위로 전달된다 (`std::move`)

**다음**: [04. CodeGen](04-codegen.md) — 트리를 LLVM IR로
