# 01. Lexer — 문자열을 토큰으로

**파일**: `include/SourceLocation.h`, `include/Lexer.h`, `src/Lexer.cpp`
**원본 튜토리얼**: Chapter 1 — *Kaleidoscope Introduction and the Lexer*

---

## 1. 하는 일

문자를 하나씩 읽어서 의미 있는 덩어리(**토큰**)로 자릅니다.

```
입력:  def foo(x) x + 1;

출력:  tok_def
       tok_identifier   IdentifierStr = "foo"
       '('
       tok_identifier   IdentifierStr = "x"
       ')'
       tok_identifier   IdentifierStr = "x"
       '+'
       tok_number       NumVal = 1.0
       ';'
       tok_eof
```

Lexer는 **문법을 전혀 모릅니다.** `def foo(` 가 올바른 순서인지, 괄호가 짝이
맞는지 판단하지 않습니다. 그건 Parser의 일입니다. Lexer는 그저 "여기까지가
식별자, 여기부터가 숫자"만 구분합니다.

---

## 2. 토큰을 `int`로 표현하는 트릭

`include/Lexer.h`:

```cpp
enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,

  // control
  tok_if = -6,   tok_then = -7,  tok_else = -8,
  tok_for = -9,  tok_in = -10,

  // operators
  tok_binary = -11,  tok_unary = -12,

  // var definition
  tok_var = -13
};
```

값이 전부 **음수**인 것이 핵심입니다. `gettok()`의 반환 타입은 `Token`이 아니라
`int`인데, 규칙이 이렇습니다.

| 반환값 | 의미 |
| --- | --- |
| 음수 | 위 표의 토큰 중 하나 |
| 양수 | 그 문자의 ASCII 코드 그대로 (`'+'` = 43, `'('` = 40 …) |

즉 **하나의 `int`에 "알려진 토큰"과 "임의의 문자"를 같이 담습니다.**

이 덕분에 Parser가 이렇게 쓸 수 있습니다.

```cpp
switch (CurTok) {
case tok_identifier:  return parseIdentifierExpr();
case tok_number:      return parseNumberExpr();
case '(':             return parseParenExpr();   // 문자와 토큰을 같은 switch에서
}
```

> **왜 `enum class`가 아닌가**
> 현대 C++에서는 보통 더 안전한 `enum class`를 권합니다. 값이 자동으로 `int`로
> 변환되지 않아 실수를 막아주기 때문입니다. 그런데 이 코드는 바로 그 자동 변환에
> 의존합니다. `enum class`로 바꾸면 `CurTok`과 `'('`를 비교할 때마다 명시적
> 형변환(`static_cast`)을 써야 해서 파서가 훨씬 지저분해집니다.
> 그래서 **의도적으로** 평범한 `enum`을 유지했습니다.
>
> 이것이 Ch1의 사소해 보이는 결정이 뒤 챕터 전체를 좌우하는 예입니다.

---

## 3. 소스 위치 추적 — `SourceLocation.h`

```cpp
struct SourceLocation {
  int Line = 1;
  int Col = 0;
};
```

`struct`는 `class`와 거의 같지만 기본이 `public`입니다. 단순 데이터 묶음에는
관례적으로 `struct`를 씁니다.

`= 1`, `= 0` 은 **기본 멤버 초기화**입니다. 따로 지정하지 않으면 이 값으로
시작합니다.

이 정보는 나중에 디버그 정보(DWARF)를 만들 때 "이 명령어는 소스 3번째 줄
11번째 칸에서 왔다"를 기록하는 데 쓰입니다 → [05-debuginfo](05-debuginfo.md)

---

## 4. Lexer 클래스

```cpp
class Lexer {
  std::istream &In;          // 읽어들일 입력 스트림
  int LastChar = ' ';        // 아직 처리하지 않은 문자 (1글자 미리보기)

  SourceLocation LexLoc{1, 0};  // 지금 읽고 있는 위치
  SourceLocation CurLoc;        // 마지막으로 반환한 토큰이 시작된 위치

  std::string IdentifierStr;    // tok_identifier일 때 채워짐
  double NumVal = 0.0;          // tok_number일 때 채워짐

  int advance();

public:
  explicit Lexer(std::istream &In) : In(In) {}

  int gettok();

  const std::string &getIdentifierStr() const { return IdentifierStr; }
  double getNumVal() const { return NumVal; }
  SourceLocation getCurLoc() const { return CurLoc; }
};
```

### 4.1 왜 `std::istream &` 인가 (튜토리얼과 다른 점)

원본 튜토리얼은 `getchar()`로 **표준 입력에서 직접** 읽습니다.

```cpp
// 원본
static int gettok() {
  static int LastChar = ' ';
  while (isspace(LastChar))
    LastChar = getchar();      // ← 항상 stdin
```

이 저장소는 생성자로 스트림을 받습니다. 이점이 셋입니다.

1. **파일과 stdin을 같은 코드로 처리** — REPL은 `std::cin`, `-c` 모드는
   `std::ifstream`
2. **테스트 가능** — `std::istringstream`에 문자열을 넣어 프로그램 실행 없이
   Lexer만 검사할 수 있습니다. 실제로 `tests/LexerTests.cpp`가 이렇게 합니다
3. 전역 상태가 사라짐 — Lexer 객체를 여러 개 만들어도 서로 간섭하지 않음

```cpp
// tests/LexerTests.cpp — 이게 가능해진 이유
std::istringstream IS("def foo(x) x+1;");
Lexer Lex(IS);
CHECK_EQ(Lex.gettok(), tok_def);
```

원본 방식으로는 프로그램 전체에 텍스트를 파이프로 넣고 눈으로 출력을 확인하는
수밖에 없습니다.

### 4.2 `LastChar` — 1글자 미리보기

Lexer는 항상 **아직 처리하지 않은 문자 하나**를 손에 들고 있습니다.

식별자를 읽는 상황을 생각해 보세요. `foo(` 에서 `foo`가 끝났다는 걸 알려면
`(` 를 읽어봐야 합니다. 그런데 `(` 는 다음 토큰의 일부이므로 버리면 안 됩니다.
그래서 `LastChar`에 보관해 뒀다가 다음 `gettok()` 호출에서 씁니다.

초깃값이 `' '`(공백)인 것도 요령입니다. 첫 호출에서 "공백은 건너뛴다" 루프에
자연스럽게 들어가 첫 글자를 읽게 됩니다.

### 4.3 접근자(getter)

`getIdentifierStr()`, `getNumVal()`, `getCurLoc()` — Parser가 Lexer에게서
필요로 하는 것은 이 셋과 `gettok()`뿐입니다. 나머지는 전부 `private`입니다.

`const std::string &` 를 반환하는 이유: 문자열을 **복사하지 않고 빌려주기**
위해서입니다. `const`가 붙어 있으니 받는 쪽이 수정할 수는 없습니다.

---

## 5. `src/Lexer.cpp` 코드 읽기

### 5.1 `advance()` — 위치를 세면서 한 글자 읽기

```cpp
int Lexer::advance() {
  int C = In.get();
  if (C == std::istream::traits_type::eof())
    C = EOF;

  if (C == '\n' || C == '\r') {
    LexLoc.Line++;
    LexLoc.Col = 0;
  } else
    LexLoc.Col++;
  return C;
}
```

`In.get()` 이 한 글자를 읽습니다. 입력이 끝나면 스트림 고유의 EOF 값을
반환하는데, 이후 코드가 `EOF` 매크로와 비교하므로 여기서 맞춰 줍니다.

줄바꿈이면 줄 번호를 올리고 칸을 0으로, 아니면 칸만 올립니다. 이게 디버그
정보에 들어갈 위치 정보의 전부입니다.

### 5.2 `gettok()` — 본체

큰 `if` 사슬입니다. 순서대로 보겠습니다.

**(a) 공백 건너뛰기**

```cpp
while (isspace(LastChar))
  LastChar = advance();

CurLoc = LexLoc;    // 토큰이 "여기서" 시작함을 기록
```

공백을 다 지난 지점이 곧 토큰의 시작 위치입니다.

**(b) 식별자와 키워드**

```cpp
if (isalpha(LastChar)) {              // [a-zA-Z]로 시작하면
  IdentifierStr = static_cast<char>(LastChar);
  while (isalnum((LastChar = advance())))   // [a-zA-Z0-9]가 계속되는 동안
    IdentifierStr += static_cast<char>(LastChar);

  if (IdentifierStr == "def")    return tok_def;
  if (IdentifierStr == "extern") return tok_extern;
  if (IdentifierStr == "if")     return tok_if;
  ...
  return tok_identifier;
}
```

먼저 글자를 다 모은 **다음에** 키워드인지 비교합니다. 그래서 `define`은
`def` + `ine`가 아니라 하나의 식별자가 됩니다. (`tests/LexerTests.cpp`가
이걸 검사합니다.)

`while (isalnum((LastChar = advance())))` 는 C 계열 특유의 압축 표현입니다.
풀어 쓰면:

```cpp
while (true) {
  LastChar = advance();        // 한 글자 읽어서 LastChar에 저장하고
  if (!isalnum(LastChar))      // 그 값이 영숫자가 아니면
    break;                     // 멈춤
  IdentifierStr += LastChar;
}
```

> **주의**: 식별자 규칙은 `[a-zA-Z][a-zA-Z0-9]*` 이고 **`_`는 포함되지
> 않습니다.** 그래서 `a_b`는 식별자 하나가 아니라 `a`, `'_'`, `b` 세 토큰이
> 됩니다. 흔히 놓치는 부분이라 테스트로 고정해 뒀습니다.

**(c) 숫자 — 튜토리얼의 버그를 고친 부분**

```cpp
if (isdigit(LastChar) || LastChar == '.') {
  std::string NumStr;
  do {
    NumStr += static_cast<char>(LastChar);
    LastChar = advance();
  } while (isdigit(LastChar) || LastChar == '.');

  const char *Start = NumStr.c_str();
  char *End = nullptr;
  NumVal = strtod(Start, &End);

  if (End != Start + NumStr.size() || End == Start) {
    fprintf(stderr, "Error: invalid number literal '%s' at %d:%d\n",
            NumStr.c_str(), CurLoc.Line, CurLoc.Col);
    NumVal = 0.0;
    HadError = true;
  }
  return tok_number;
}
```

`strtod`는 문자열을 `double`로 바꾸는 C 표준 함수입니다. 두 번째 인자로
포인터의 주소를 주면 **"어디까지 읽었는지"** 를 알려줍니다.

원본 튜토리얼은 이 정보를 버립니다.

```cpp
// 원본
NumVal = strtod(NumStr.c_str(), 0);   // 어디서 멈췄는지 무시
```

그래서 `1.23.45.67`을 넣으면 `strtod`가 두 번째 `.`에서 멈추고 **조용히**
`1.23`을 돌려줍니다. 나머지는 사라지는데 아무 경고도 없습니다. 튜토리얼
본문도 이 문제를 인정하지만 고치지는 않습니다.

이 저장소는 끝 위치를 검사해서 오류로 처리합니다.

```
$ echo '1.23.45.67;' | ./build/toy
Error: invalid number literal '1.23.45.67' at 1:1
```

두 조건의 의미:
- `End != Start + NumStr.size()` — 끝까지 못 읽음 = 뒤에 쓰레기가 있음
- `End == Start` — 하나도 못 읽음 = `.` 하나만 있는 경우

### 진단만으로는 부족하다 — `hadError()`

거부된 토큰도 `tok_number`를 반환하고 `NumVal`은 0.0입니다. 렉싱을 계속하기
위해서인데, 그래서 **파서는 멀쩡한 숫자를 본 것과 구별하지 못합니다.** 처음
구현에는 진단 출력만 있어서 이런 일이 벌어졌습니다.

```
$ ./build/toy -c bad.ks -o bad.o     # def bad() 1.23.45;
Error: invalid number literal '1.23.45' at 1:11
Wrote bad.o                           ← 오브젝트가 생성됨
$ echo $?
0                                     ← 종료 코드 0
```

생성된 IR은 `ret double 0.000000e+00`입니다. 컴파일러가 스스로 틀렸다고
말해놓고 결과물을 내놓은 셈입니다. IR 테스트(`test/driver/bad-number.ks`)를
붙이는 과정에서 드러난 문제입니다.

그래서 Lexer가 실패 사실을 기록하고,

```cpp
// Lexer.h
bool hadError() const { return HadError; }
```

`-c` 드라이버가 오브젝트를 쓰기 전에 확인합니다.

```cpp
// main.cpp -- runCompile()
if (Lex.hadError())
  Failed = true;
```

REPL은 확인하지 않습니다. 한 줄이 잘못됐다고 세션을 끝낼 이유가 없고, 다음
입력은 정상적으로 처리하면 됩니다. **같은 오류가 모드에 따라 다르게 처리되는
것이 의도**입니다.

> **설계 선택**: `1.2.3`을 `1.2`와 `.3` 두 토큰으로 쪼갤 수도 있었습니다.
> 그러면 오타 하나가 파서 단계에서 알 수 없는 에러 여러 개로 번집니다.
> 통째로 한 토큰으로 두고 여기서 한 번만 정확히 알려주는 쪽을 택했습니다.

**(d) 주석**

```cpp
if (LastChar == '#') {
  do
    LastChar = advance();
  while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

  if (LastChar != EOF)
    return gettok();     // ← 자기 자신을 다시 호출
}
```

`#`부터 줄 끝까지 버립니다. 주석은 토큰을 만들지 않으므로, 다 버린 뒤
`gettok()`을 **재귀 호출**해서 다음 진짜 토큰을 가져옵니다.

**(e) EOF와 그 외 문자**

```cpp
if (LastChar == EOF)
  return tok_eof;        // EOF는 소비하지 않음

int ThisChar = LastChar;
LastChar = advance();
return ThisChar;         // 그 외에는 문자를 ASCII 값 그대로
```

EOF를 소비하지 않는 것이 중요합니다. 그래서 `gettok()`을 몇 번을 더 불러도
계속 `tok_eof`가 나오고, 입력 끝을 넘어가지 않습니다.

마지막 줄이 2절에서 말한 "양수 = 문자 그대로"를 만들어 내는 지점입니다.

### 5.3 `getTokName()`

토큰 번호를 사람이 읽을 문자열로 바꿉니다. 진단용이며 현재 컴파일 흐름에서는
쓰이지 않습니다. 원본 튜토리얼 Ch9에 있던 것을 그대로 가져왔습니다.

---

## 6. 직접 확인해 보기

```bash
conda activate llvm-tut
cmake -S . -B build -G Ninja && cmake --build build

# Lexer 단위 테스트 (LLVM 없이 Lexer.cpp만 링크됨)
./build/lexer_tests
```

테스트 5개가 다음을 검사합니다.

| 테스트 | 내용 |
| --- | --- |
| keywords and identifiers | 키워드 10개 인식, `define`은 식별자, `_`는 식별자 문자가 아님 |
| numbers | `3`, `3.25`, `.5`, `42.` 정상 / `1.23.45.67`, `.` 거부, `hadError()` 표시 |
| operators are raw ascii | `+-*/<>=(),;\|&:!` 가 각자의 ASCII 값으로 나옴 |
| comments and eof | 주석 무시, 빈 입력, EOF를 여러 번 물어봐도 안전 |
| source locations | 줄/칸 번호가 올바르게 증가 |

---

## 7. 정리

- Lexer는 문법을 모른다. 문자 → 토큰 변환만 한다
- 토큰은 `int`. **음수 = 토큰, 양수 = 문자 그대로**. 이 규칙이 파서 전체를 좌우한다
- `LastChar` 한 글자 미리보기로 토큰 경계를 판단한다
- `std::istream&`을 받도록 바꾼 덕분에 단위 테스트가 가능해졌다
- 숫자 리터럴 검증은 튜토리얼에 없는 수정이다
- 거부된 토큰도 값을 돌려주므로, 실패는 `hadError()`로만 드러난다.
  `-c`는 이걸 확인해 오브젝트를 안 쓰고, REPL은 무시하고 계속한다

**다음**: [02. AST](02-ast.md) — 토큰으로 만들 트리의 모양
