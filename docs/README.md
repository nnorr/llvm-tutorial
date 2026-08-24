# 컴포넌트별 코드 설명

이 디렉터리는 `include/`, `src/` 아래 각 모듈이 **실제로 어떻게 구현돼 있는지**
줄 단위로 따라가며 설명합니다.

- 설계 의도와 모듈 간 관계 → [`../ARCHITECTURE.md`](../ARCHITECTURE.md)
- 빌드/실행 방법 → [`../README.md`](../README.md)
- 이 디렉터리 → **코드 자체를 읽는 법**

C++에 익숙하지 않은 독자를 기준으로 썼습니다. 새로 나오는 문법은 처음 등장하는
곳에서 설명하고, 자주 쓰이는 것들은 아래 "미리 알아둘 C++ 문법"에 모아뒀습니다.

## 읽는 순서

원본 튜토리얼([LLVM Kaleidoscope](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/index.html))의
챕터 순서와 맞춰 읽는 것을 권합니다.

| 문서 | 다루는 파일 | 원본 튜토리얼 |
| --- | --- | --- |
| [01. Lexer](01-lexer.md) | `Lexer.{h,cpp}`, `SourceLocation.h` | Ch1 — Kaleidoscope Introduction and the Lexer |
| [02. AST](02-ast.md) | `AST.h`, `ASTVisitor.h`, `ASTDumper.{h,cpp}` | Ch2 — Implementing a Parser and AST |
| [03. Parser](03-parser.md) | `Parser.{h,cpp}`, `OperatorTable.h` | Ch2, Ch5, Ch6, Ch7 |
| [04. CodeGen](04-codegen.md) | `CodeGen.{h,cpp}` | Ch3, Ch4, Ch5, Ch6, Ch7 |
| [05. DebugInfo](05-debuginfo.md) | `DebugInfo.{h,cpp}` | Ch9 — Adding Debug Information |
| [06. Backend & Driver](06-backend-and-driver.md) | `ObjectEmitter.{h,cpp}`, `main.cpp` | Ch8 — Compiling to Object Code |
| [07. 최적화 패스](07-passes.md) | `CodeGen::initModule`, `ObjectEmitter` | Ch4 — Adding JIT and Optimizer Support |
| [08. JIT 구조](08-jit.md) | `KaleidoscopeJIT.h`, `main.cpp` | Ch4, 별도 시리즈 *Building a JIT* |
| [09. Visitor 설계 변천](09-visitor-evolution.md) | `ASTVisitor.h`, `AST.h` | — (이 저장소 고유) |
| [10. 디버그 정보와 최적화](10-debuginfo-and-optimization.md) | `main.cpp`, `CodeGen.cpp` | — (이 저장소 고유) |

한 페이지로 압축한 정리는 [SUMMARY.md](SUMMARY.md)에 있습니다 — 컴포넌트, 구현,
설계 결정, CRTP까지 한 번에 훑는 용도입니다.

01–06은 "코드가 무엇을 하는가", 07–08은 **LLVM 인프라를 어떻게 쓰는가**를
다룹니다. 멘토가 지목한 *Function Pass* 와 *JIT 컴파일러 구조* 가 이 둘입니다.

09와 10은 부록입니다. 09는 AST 순회 방식을 두 번 갈아엎은 기록 — 버린 설계가
왜 그렇게 생겼고 무엇 때문에 버렸는지의 비교입니다. Expression Problem과
MLIR의 dialect 확장성이 왜 다른 축의 문제인지도 여기서 다룹니다.
10은 05와 07이 "`-g`면 최적화를 끈다" 한 줄로 넘어간 부분을 펼쳐서,
그 제약이 어디서 오는지와 clang/gcc가 `-O2 -g`를 어떻게 지원하는지를 봅니다.

## 전체 흐름 한눈에

```
소스 텍스트
   │
   │  Lexer          문자열 → 토큰 (def, 식별자, 숫자, '+' ...)
   ▼
 토큰 스트림
   │
   │  Parser         토큰 → 트리 구조
   ▼
  AST (트리)
   │
   │  CodeGen        트리 → LLVM IR (중간 표현)
   ▼
 LLVM IR
   │
   ├─ JIT            메모리에서 바로 실행       ← 기본 모드
   └─ ObjectEmitter  .o 파일로 출력             ← -c 모드
```

예를 들어 `1 + 2 * 3` 은 이렇게 변환됩니다.

```
텍스트   "1 + 2 * 3"
  ↓ Lexer
토큰     [number(1)] [+] [number(2)] [*] [number(3)]
  ↓ Parser                     ('*'가 '+'보다 우선순위가 높으므로)
AST      Binary '+'
           ├── Number 1
           └── Binary '*'
                 ├── Number 2
                 └── Number 3
  ↓ CodeGen
IR       %multmp = fmul double 2.0, 3.0
         %addtmp = fadd double 1.0, %multmp
```

---

## 미리 알아둘 C++ 문법

이 코드베이스를 읽는 데 필요한 최소한만 정리했습니다. 이미 아는 내용은 건너뛰세요.

### 헤더(.h)와 소스(.cpp)

C++은 파일을 둘로 나눕니다.

- **헤더 (`.h`)** — "무엇이 있는지" 선언만. 다른 파일이 `#include`로 가져다 씀
- **소스 (`.cpp`)** — "실제로 어떻게 동작하는지" 구현

```cpp
// Lexer.h  — 이런 함수가 있다고 알림
int gettok();

// Lexer.cpp — 실제 내용
int Lexer::gettok() { /* ... 100줄 ... */ }
```

`Lexer::` 는 "이 함수는 `Lexer` 클래스에 속한다"는 표시입니다.

헤더 맨 위의 `#ifndef KALEIDOSCOPE_LEXER_H / #define ... / #endif` 는
**include guard**입니다. 같은 헤더가 두 번 포함돼도 내용이 한 번만 처리되게 합니다.

### 클래스

데이터(멤버 변수)와 그 데이터를 다루는 함수(멤버 함수)를 묶은 것입니다.

```cpp
class Lexer {
  std::string IdentifierStr;   // private: 클래스 밖에서 접근 불가 (기본값)
public:
  int gettok();                // public: 밖에서 호출 가능
};
```

`private`가 기본이라 `class` 바로 아래 멤버들은 외부에서 못 건드립니다.
`public:` 아래부터가 외부 인터페이스입니다.

### 생성자와 초기화 리스트

객체가 만들어질 때 자동 호출되는 함수입니다. 클래스 이름과 같습니다.

```cpp
explicit Lexer(std::istream &In) : In(In) {}
//                                 ^^^^^^^ 초기화 리스트
```

`: In(In)` 은 "멤버 변수 `In`을 매개변수 `In`으로 초기화" 라는 뜻입니다.
`explicit`은 의도치 않은 자동 형변환을 막는 안전장치입니다.

### 참조(`&`)와 포인터(`*`)

둘 다 "다른 곳에 있는 값을 가리킨다"는 뜻이지만 쓰임이 다릅니다.

| | 표기 | 비어 있을 수 있나 | 이 코드에서의 의미 |
| --- | --- | --- | --- |
| 참조 | `Lexer &Lex` | 아니오 | "반드시 있는 것을 빌려 씀" |
| 포인터 | `DebugInfo *Dbg` | 예 (`nullptr`) | "있을 수도, 없을 수도" |

그래서 `CodeGen`이 `OperatorTable &Ops`(참조)와 `DebugInfo *Dbg`(포인터)를
다르게 가진 것은 의도적입니다. 연산자 테이블은 항상 필요하고, 디버그 정보는
`-g`일 때만 있습니다.

`*Ptr`는 "포인터가 가리키는 실제 값", `&Value`는 "값의 주소"입니다.

### `const`

"이건 안 바꾼다"는 약속입니다.

```cpp
double getNumVal() const { return NumVal; }
//                 ^^^^^ 이 함수는 멤버 변수를 수정하지 않음
```

### 상속과 가상 함수 (virtual)

이 코드의 핵심 문법입니다.

```cpp
class ExprAST {                          // 부모 (base class)
public:
  virtual ~ExprAST() = default;          // 가상 소멸자
  ExprASTKind getKind() const { return Kind; }
};

class NumberExprAST : public ExprAST {   // 자식
  double Val;
public:
  double getVal() const { return Val; }
};
```

- `class 자식 : public 부모` — 자식은 부모의 멤버를 물려받고, 부모 타입의
  참조/포인터에 담을 수 있습니다. 그래서 `std::unique_ptr<ExprAST>` 하나로
  9가지 노드를 모두 담습니다
- `virtual` — "자식이 이 함수를 바꿔 정의할 수 있다"
- `= 0` (순수 가상) — "부모는 구현이 없다. 자식이 반드시 구현해야 한다".
  이런 함수가 하나라도 있으면 그 클래스는 직접 객체를 만들 수 없습니다
  (추상 클래스)
- `override` — 부모의 가상 함수를 재정의하는 중이라고 컴파일러에게 알림

**이 저장소에서 `virtual`은 소멸자에만 쓰입니다.** 이유는 아래 "소멸자"
항목에 있고, 노드 종류에 따라 다르게 처리하는 일은 가상 함수가 아니라
`getKind()` 스위치로 합니다 ([02. AST](02-ast.md) 4절). 왜 그 선택을
했는지는 [09. Visitor 설계 변천](09-visitor-evolution.md)에서 다룹니다.

### 소멸자

객체가 사라질 때 자동 호출됩니다. `~클래스이름()` 형태입니다.

```cpp
virtual ~ExprAST() = default;
```

`virtual`이 붙은 이유: 부모 포인터로 자식 객체를 삭제할 때 자식의 소멸자까지
제대로 불리게 하기 위해서입니다. 없으면 메모리 누수가 납니다.
`= default`는 "특별히 할 일 없으니 컴파일러가 알아서"라는 뜻입니다.

### `std::unique_ptr` 과 소유권

C++에는 자동 메모리 회수(GC)가 없어서, 누가 메모리를 해제할지 정해야 합니다.
`unique_ptr`은 **"이 포인터가 유일한 주인이고, 주인이 사라지면 메모리도 해제"**
를 자동으로 보장합니다.

```cpp
std::unique_ptr<ExprAST> LHS;   // LHS가 사라지면 가리키던 노드도 자동 삭제
```

중요한 규칙: **복사할 수 없습니다.** 주인이 둘이면 두 번 해제되기 때문입니다.
대신 **옮길(move)** 수 있습니다.

```cpp
std::move(LHS)   // "LHS의 소유권을 넘긴다". 이후 LHS는 비어 있음(nullptr)
```

그래서 파서 코드에 `std::move`가 잔뜩 나옵니다. AST 노드를 만들면서 자식
노드의 소유권을 부모에게 넘기는 것입니다.

> `std::move` 이후에 원래 변수를 쓰면 버그입니다. 실제로 원본 튜토리얼에
> 이 버그가 있었고, 이 저장소에서는 고쳤습니다 (04-codegen 문서 참고).

### 표준 컨테이너

| 타입 | 뜻 | 예 |
| --- | --- | --- |
| `std::string` | 문자열 | `"binary\|"` |
| `std::vector<T>` | 가변 길이 배열 | `std::vector<std::string> Args` |
| `std::map<K,V>` | 키→값 사전 (정렬됨) | `std::map<char,int> Precedence` |
| `std::pair<A,B>` | 두 값 묶음 | `(이름, 초기값)` |

`std::map`에서 `M[key]` 는 키가 없으면 **기본값을 만들어 넣습니다.**
포인터라면 `nullptr`이 됩니다. 이 성질을 코드가 실제로 활용합니다.

### `enum`

이름 붙인 정수 상수 묶음입니다.

```cpp
enum Token { tok_eof = -1, tok_def = -2, ... };
```

이 프로젝트에서 `Token`을 (더 안전한) `enum class`가 아닌 평범한 `enum`으로
둔 데는 이유가 있습니다 → [01-lexer](01-lexer.md) 참고.

### 템플릿의 `<>`

`IRBuilder<>` 처럼 붙는 꺾쇠는 "타입을 매개변수로 받는 코드"라는 뜻입니다.
이 프로젝트에서는 LLVM이 제공하는 것을 쓰기만 하므로, `<>`가 보이면
"기본 설정으로 쓴다" 정도로 이해하면 충분합니다.

### 네임스페이스

이름 충돌을 막는 구역입니다. 이 프로젝트 코드는 전부 `kaleidoscope` 안에,
LLVM 코드는 `llvm` 안에 있습니다.

```cpp
namespace kaleidoscope { ... }   // 우리 코드
llvm::Value *V;                  // LLVM의 Value
using namespace llvm;            // 이후 llvm:: 생략 가능
```

`.cpp` 파일 안에서 이름 없는 `namespace { ... }` 는 "이 파일 안에서만 쓰는
것"이라는 뜻입니다. 헤더에서 쓰면 안 되는 이유는
[02-ast](02-ast.md)에 설명돼 있습니다.
