# 11. 테스트 — lit과 FileCheck로 IR 고정하기

**파일**: `test/lit.cfg.py`, `test/lit.site.cfg.py.in`, `test/**/*.ks`,
`tests/LexerTests.cpp`, `tests/RunJit.cmake`, `CMakeLists.txt`
**LLVM 도구**: `lit`, `FileCheck`, `not`
**관련 문서**: [07. 최적화 패스](07-passes.md), [10. 디버그 정보와 최적화](10-debuginfo-and-optimization.md)

컴파일러 테스트는 보통의 프로그램 테스트와 다릅니다. `fib(10)`이 89를 돌려주는지
확인하는 것으로는 부족합니다. 그건 **인터프리터라도 통과**하기 때문입니다.
프론트엔드의 진짜 계약은 "돌아가는가"가 아니라 **"어떤 IR을 내놓는가"** 입니다.

- mem2reg가 정말로 `alloca`를 없앴는가
- `-g`를 줬을 때 그 `alloca`가 정말로 **살아남았는가** ([10번 문서](10-debuginfo-and-optimization.md)의 주제)
- 사용자 정의 연산자가 정말로 직접 호출로 낮춰졌는가

이건 전부 IR 텍스트를 봐야 알 수 있고, 그래서 LLVM에는 이걸 위한 도구가 따로
있습니다. 이 문서는 그 도구들(`lit`, `FileCheck`, `not`)이 무엇이고 이 저장소에
어떻게 배선돼 있는지를 다룹니다.

---

## 1. 테스트 세 층

```bash
ctest --test-dir build --output-on-failure
```

```
1/4 Test #1: lexer ............................   Passed
2/4 Test #2: jit_fib ..........................   Passed
3/4 Test #3: jit_operators ....................   Passed
4/4 Test #4: lit ..............................   Passed
```

| 스위트 | 무엇을 고정하나 | 도구 |
| --- | --- | --- |
| `lexer` | 토큰화 규칙 (단위 테스트 5개, 검사 87개) | 없음 — 매크로 몇 개 |
| `jit_fib`, `jit_operators` | 종단간 실행 결과 (89 / 30) | `cmake -P` 스크립트 |
| `lit` | **생성된 IR** (테스트 10개) | **`lit` + `FileCheck`** |

앞의 세 개는 "컴파일러가 도는가"를 봅니다. 이 문서의 대부분은 네 번째,
즉 IR을 보는 층에 대한 것입니다.

---

## 2. 왜 gtest가 아니라 lit + FileCheck인가

C++ 테스트 프레임워크로 IR을 검사하려면 이렇게 됩니다.

```cpp
// 이렇게 하고 싶지 않다
std::string IR = compileToString("def fib(n) var a = 0 in ...");
EXPECT_TRUE(IR.find("phi double") != std::string::npos);
EXPECT_TRUE(IR.find("alloca") == std::string::npos);
```

테스트 하나 추가할 때마다 C++을 고치고 재컴파일해야 하고, 문자열 검색은
**순서**를 못 봅니다. 컴파일러 테스트의 입력과 출력은 둘 다 텍스트인데
굳이 C++을 경유할 이유가 없습니다.

LLVM은 그래서 다른 방식을 씁니다. `llvm/test` 아래 3만 개가 넘는 테스트가
**전부 이 형식**입니다.

| 도구 | 역할 |
| --- | --- |
| **`lit`** (LLVM Integrated Tester) | 테스트 러너. 파일 안에서 `RUN:` 줄을 찾아 셸 명령으로 실행하고, 종료 코드로 합격/불합격을 판정 |
| **`FileCheck`** | 패턴 매처. 표준 입력을 받아 `CHECK:` 줄과 **순서대로** 대조 |
| **`not`** | 앞 명령이 실패해야 통과시키는 래퍼. 오류 경로 테스트용 |

핵심은 **테스트가 코드가 아니라 데이터**라는 점입니다. 테스트 파일 하나가
자기 실행 방법(`RUN:`)과 기대값(`CHECK:`)을 같이 들고 있고, 추가는 파일을
하나 놓는 것으로 끝입니다. 재빌드가 없습니다.

---

## 3. 테스트 파일 하나 뜯어보기

`test/codegen/mem2reg.ks` 전문입니다.

```
# Mutable variables are emitted as stack slots and promoted by mem2reg, so no
# alloca survives and the loop carries its state in PHI nodes instead.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def binary : 1 (x y) y;
def fib(n)
  var a = 0, b = 1, c in
  (for i = 1, i < n in
     (c = a + b : a = b : b = c)) : b;

# CHECK-LABEL: define double @fib(double %n)
# CHECK-NOT:     alloca
# CHECK:         phi double
# CHECK:         phi double
# CHECK:         phi double
# CHECK:         fadd double
```

파일 하나가 세 가지를 겸합니다.

- **Kaleidoscope 소스** — 가운데 `def` 두 개
- **실행 스크립트** — `RUN:` 줄
- **기대값 명세** — `CHECK:` 줄들

`#`이 Kaleidoscope의 주석 문자라서 이게 가능합니다. 컴파일러는 `RUN:`/`CHECK:`
줄을 주석으로 보고 무시하고, `lit`과 `FileCheck`는 파일 전체를 텍스트로 훑으며
자기가 찾는 표시만 골라냅니다. 서로를 모르는 채 같은 파일을 공유합니다.

### 3.1 `RUN:` 줄

```
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s
```

`%`로 시작하는 것이 **치환(substitution)** 입니다. `lit`이 실행 직전에 실제
경로로 바꿔치기합니다.

| 치환 | 값 | 어디서 정의 |
| --- | --- | --- |
| `%s` | 테스트 파일의 절대 경로 | lit 내장 |
| `%t` | 이 테스트 전용 임시 파일 이름 | lit 내장 |
| `%toy` | 빌드된 `build/toy` 경로 | `lit.cfg.py` (5절) |
| `%filecheck` | `FileCheck` 실행 파일 경로 | `lit.cfg.py` (5절) |

치환 후 실제로 도는 명령은 이렇게 됩니다.

```bash
/…/build/toy -c /…/test/codegen/mem2reg.ks --emit-llvm -o /dev/null 2>&1 \
  | /…/libexec/llvm/FileCheck /…/test/codegen/mem2reg.ks
```

세 부분이 의도적입니다.

- `-o /dev/null` — 우리가 볼 건 IR이지 오브젝트 파일이 아닙니다. 하지만
  `--emit-llvm`은 `-c` 모드 전용이라 `-o`가 필요하므로 버립니다
- `2>&1` — `--emit-llvm`의 IR은 `Module::print(errs(), …)`로 **stderr에**
  나갑니다 (`src/main.cpp`의 `emitModuleIR`). 파이프는 stdout만 넘기므로
  합쳐줘야 `FileCheck`가 받습니다
- 마지막 `%s` — `FileCheck`의 인자는 **검사할 입력이 아니라 `CHECK:` 줄이
  들어 있는 파일**입니다. 입력은 stdin으로 받습니다. 그래서 같은 파일이
  두 번 등장합니다

### 3.2 `CHECK:` 줄

`FileCheck`는 grep이 아닙니다. **입력을 한 방향으로만 훑으면서** `CHECK:` 줄을
차례로 소비합니다. 위 테스트는 이렇게 읽힙니다.

```
"define double @fib(double %n)" 을 찾는다                    ← CHECK-LABEL
  그 다음부터 "phi double" 을 만나기 전까지 "alloca" 가 없어야 한다  ← CHECK-NOT
  "phi double" 이 세 번 나온다 (a, b, i)                      ← CHECK ×3
  그 다음 "fadd double" 이 나온다                             ← CHECK
```

실제 IR과 맞춰 보면:

```llvm
define double @fib(double %n) {          ← CHECK-LABEL 여기 매치
entry:
  br label %loop
                                          ← 이 구간에 alloca 가 없어야 함
loop:
  %a.0 = phi double [ 0.0, %entry ], [ %b.0, %loop ]     ← CHECK
  %b.0 = phi double [ 1.0, %entry ], [ %addtmp, %loop ]  ← CHECK
  %i.0 = phi double [ 1.0, %entry ], [ %nextvar, %loop ] ← CHECK
  %addtmp = fadd double %a.0, %b.0                       ← CHECK
  …
```

`var a = 0, b = 1, c` 는 코드젠 단계에서 `alloca` 세 개로 나옵니다
([07번 문서](07-passes.md) 6절). 그걸 `PromotePass`가 PHI로 바꾼다는 것이
이 테스트가 고정하는 사실입니다. 패스를 빼면 `CHECK-NOT: alloca`가 즉시 깨집니다.

---

## 4. FileCheck 지시자

이 저장소가 쓰는 것들입니다.

| 지시자 | 의미 |
| --- | --- |
| `CHECK:` | 현재 위치 **이후** 어딘가에 이 문자열이 있어야 한다 |
| `CHECK-LABEL:` | 같은 뜻이지만, 입력을 **블록으로 쪼개는 경계** 역할도 한다 |
| `CHECK-NOT:` | 앞뒤 매치 **사이 구간에** 이 문자열이 없어야 한다 |
| `CHECK-SAME:` | 바로 앞 매치와 **같은 줄에** 있어야 한다 |
| `{{정규식}}` | 그 자리를 정규식으로 매치 (SSA 이름처럼 매번 달라지는 것) |

LLVM에는 이 밖에도 `CHECK-NEXT:`(바로 다음 줄), `CHECK-DAG:`(순서 무관),
`[[VAR:패턴]]` / `[[VAR]]`(값을 잡아 뒤에서 재사용) 같은 것이 있습니다.
여기서는 필요가 없어 안 씁니다.

### 4.1 `CHECK-LABEL`이 따로 있는 이유

`CHECK-LABEL`은 두 단계로 동작합니다. 먼저 입력 전체에서 라벨들을 찾아
**블록 경계**를 만들고, 그 다음 각 블록 안에서만 나머지 `CHECK:`를 대조합니다.

효과가 두 가지입니다.

- **오염 방지** — `@fib`용 `CHECK:`가 실수로 `@main`의 코드에 매치되는 일이
  구조적으로 불가능해집니다
- **진단 개선** — 실패했을 때 "어느 함수에서 틀렸는지"가 바로 나옵니다.
  블록 경계가 없으면 FileCheck는 파일 끝까지 헤매다 엉뚱한 곳을 지목합니다

`test/codegen/user-operators.ks`가 이걸 세 개 연달아 씁니다.

```
# CHECK-LABEL: define double @"unary!"(double %v)
# CHECK-LABEL: define double @"binary>"(double %LHS, double %RHS)
# CHECK-LABEL: define double @test(double %x)
# CHECK:         call double @"binary>"(double %x, double 2.000000e+00)
# CHECK:         call double @"unary!"(
```

마지막 두 `CHECK:`는 `@test` 블록 안에서만 찾습니다. 사용자 정의 연산자가
**이름이 뭉개진 평범한 함수**가 되고, 사용처는 직접 호출로 낮춰진다는 것을
고정합니다. 참고로 `CHECK-LABEL`의 패턴은 파일 안에서 유일해야 합니다
(그래야 경계로 쓸 수 있으니).

### 4.2 `CHECK-NOT`의 범위 함정

`CHECK-NOT`은 "파일 전체에 없다"가 아니라 **"앞 매치와 뒤 매치 사이에 없다"**
입니다. 뒤에 오는 긍정 `CHECK:`가 범위의 끝을 정합니다.

```
# CHECK-LABEL: define double @loop(double %n)   ← 범위 시작
# CHECK:         fcmp ult double
# CHECK-NOT:     uitofp                          ┐ 이 두 개는
# CHECK-NOT:     fcmp one                        ┘ fcmp ult 와 br i1 사이만 본다
# CHECK:         br i1                            ← 범위 끝
```

`test/codegen/binop-fold.ks`입니다. Kaleidoscope에는 bool이 없어서 비교
결과를 `uitofp`로 double로 넓혔다가 다시 `fcmp one …, 0.0`으로 좁히는 왕복이
생기는데, InstCombine이 이걸 원래 `fcmp`로 되접습니다. 그 왕복 코드가 분기
직전 구간에 없다는 것을 확인하는 겁니다.

반대로 뒤에 긍정 `CHECK:`가 없으면 범위는 **입력 끝까지**입니다.
`test/debuginfo/no-debug-by-default.ks`가 그 형태입니다.

```
# CHECK-LABEL: define double @sq(double %x)
# CHECK-NOT:   !dbg
# CHECK-NOT:   llvm.dbg.cu
# CHECK-NOT:   DISubprogram
```

`-g` 없이는 디버그 메타데이터가 **하나도** 없어야 하므로 이게 맞습니다.

### 4.3 정규식이 필요한 자리

```
# CHECK:         fmul double {{.*}}, !dbg
```

`test/debuginfo/dwarf-metadata.ks`입니다. 실제 IR은
`%multmp = fmul double %x2, %x3, !dbg !11` 처럼 나오는데, `%x2` 같은 SSA
이름과 `!11` 같은 메타데이터 번호는 코드젠이 조금만 바뀌어도 달라집니다.
우리가 고정하고 싶은 건 "곱셈에 `!dbg`가 붙어 있다"이지 번호가 아니므로
그 사이를 `{{.*}}`로 비웁니다.

같은 파일의 `CHECK-SAME:`도 같은 맥락입니다.

```
# CHECK:       distinct !DICompileUnit(language: DW_LANG_C
# CHECK-SAME:    producer: "Kaleidoscope Compiler"
```

`!DICompileUnit(…)`은 한 줄에 필드가 열 개 넘게 들어가는 긴 줄입니다.
`CHECK-SAME`은 "앞 매치와 같은 줄에서 계속 찾아라"라는 뜻이라, 긴 한 줄을
여러 조각으로 나눠 검사하면서 그 조각들이 **같은 노드**에 속한다는 것까지
보장합니다. 그냥 `CHECK:` 두 개로 쓰면 producer가 다른 메타데이터 노드에
있어도 통과해 버립니다.

---

## 5. `not` — 실패해야 통과하는 테스트

컴파일러는 잘못된 입력을 **거부**해야 합니다. 그런데 `lit`은 명령이 0이
아닌 값으로 끝나면 테스트를 실패로 봅니다. 그래서 LLVM에는 종료 코드를
뒤집는 `not`이라는 작은 도구가 있습니다.

`test/driver/bad-number.ks`가 이걸 가장 많이 씁니다.

```
# RUN: rm -f %t.o
# RUN: not %toy -c %s -o %t.o 2>&1 | %filecheck %s
# RUN: not test -e %t.o

def bad() 1.23.45;

# CHECK: invalid number literal '1.23.45'
```

`RUN:` 줄이 여러 개면 **전부** 성공해야 하고, 순서대로 실행됩니다.
세 줄이 각각 하는 일이 다릅니다.

1. `rm -f %t.o` — 이전 실행이 남긴 파일을 지웁니다. 안 지우면 3번이
   지난번 산출물을 보고 실패합니다
2. `not %toy …` — 컴파일러가 **0이 아닌 값으로 끝나야** 통과. 그리고
   진단 메시지를 `FileCheck`로 확인
3. `not test -e %t.o` — 오브젝트 파일이 **생기지 않았어야** 통과

3번이 이 테스트의 핵심입니다. `1.23.45`는 거부되지만 Lexer는 파싱을 계속하기
위해 그 자리에 `0.0`을 돌려줍니다([01. Lexer](01-lexer.md) 참조). 그래서
파서 입장에서는 정상 리터럴과 구분이 안 되고, 드라이버가 `Lexer::hadError()`를
따로 확인하지 않으면 **"오류"라고 출력해 놓고 `.o`를 써 버립니다**.
2번만으로는 이 버그가 잡히지 않고, 3번이 있어야 잡힙니다.

> 실제로 이 테스트를 쓰다가 `hadError()`가 없던 문제를 발견했습니다.
> `ARCHITECTURE.md`의 deviation 7이 그 기록입니다.

`test/driver/g-requires-c.ks`와 `one-toplevel-only.ks`도 `not`을 씁니다.
전자는 `-g`를 REPL 모드에서 거부하는지, 후자는 최상위 식이 두 개일 때
(둘 다 `main`이 되므로) 거부하는지를 봅니다.

---

## 6. 배선 — lit 설정과 CMake

`lit`이 테스트를 찾고 `%toy`가 실제 경로로 바뀌려면 설정 파일이 필요합니다.
파일이 두 개인 데는 이유가 있습니다.

```
test/lit.cfg.py           ← 사람이 쓰는 설정. 빌드와 무관한 부분
test/lit.site.cfg.py.in   ← 템플릿. 빌드 경로가 들어갈 자리만 비어 있음
        │ CMake가 채움
        ▼
build/test/lit.site.cfg.py  ← lit이 실제로 읽는 것
```

### 6.1 `lit.cfg.py`

```python
config.name = "Kaleidoscope"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ks"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.kaleidoscope_obj_root, "test")

config.substitutions.append(("%toy", config.toy_binary))
config.substitutions.append(("%filecheck", config.filecheck_binary))

config.environment["PATH"] = os.path.pathsep.join(
    [os.path.dirname(config.filecheck_binary), config.environment.get("PATH", "")]
)
```

| 항목 | 의미 |
| --- | --- |
| `ShTest(execute_external=True)` | `RUN:` 줄을 **진짜 셸**로 실행. lit 내장 셸 대신 bash를 쓰므로 `rm`, `test -e`, 파이프가 그대로 동작 |
| `suffixes = [".ks"]` | `test/` 아래 `.ks` 파일만 테스트로 인식 |
| `test_source_root` | 테스트를 **찾을** 위치 (소스 트리) |
| `test_exec_root` | 테스트를 **돌릴** 위치. `%t` 임시 파일이 여기 생김 (빌드 트리) |

마지막 `PATH` 조작이 `not`을 해결합니다. conda-forge는 `FileCheck`를 `bin/`이
아니라 `libexec/llvm/`에 숨겨 두는데, **`not`도 같은 디렉터리에 있습니다.**

```
$CONDA_PREFIX/libexec/llvm/
├── FileCheck
├── not          ← 5절의 RUN: 줄이 이걸 씁니다
├── count
├── split-file
└── …
```

그래서 `FileCheck`의 디렉터리를 `PATH` 앞에 붙이면 `%filecheck` 치환과
`not` 해결이 한 번에 됩니다.

> **치환 순서 함정.** `%toy`는 `%t`로 시작합니다. lit이 `%t`를 먼저 처리하면
> `%toy`가 `/tmp/…/mem2reg.ks.tmpoy`가 돼 버립니다. 다행히 lit의
> `getDefaultSubstitutions()`는 `config.substitutions`를 **먼저** 넣고
> 내장 치환(`%s`, `%t`, …)을 뒤에 붙이므로 긴 쪽이 이깁니다. 의존해도 되는
> 동작이지만, 알고 있어야 하는 순서입니다.

### 6.2 CMake 2단계 생성

`CMakeLists.txt`에서 site 설정을 만드는 부분이 두 단계입니다.

```cmake
set(TOY_BINARY $<TARGET_FILE:toy>)
# Two steps: configure_file expands @VARS@ but not generator expressions,
# file(GENERATE) does the reverse. $<TARGET_FILE:toy> needs the second.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/test/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/test/lit.site.cfg.py.tmp
  @ONLY
)
file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test/lit.site.cfg.py
  INPUT  ${CMAKE_CURRENT_BINARY_DIR}/test/lit.site.cfg.py.tmp
)
```

CMake의 두 기능이 서로 다른 것을 펼치기 때문입니다.

| | `@VAR@` 치환 | 생성기 표현식 `$<…>` |
| --- | --- | --- |
| `configure_file` | O | X |
| `file(GENERATE)` | X | O |

`$<TARGET_FILE:toy>`(= `toy`의 최종 경로)는 **설정 시점에 아직 모르는 값**
입니다. 멀티 컨피그 생성기에서는 `Debug/toy`일 수도 `Release/toy`일 수도
있어서, 생성 시점까지 미뤄집니다. 그래서 `configure_file`로 `@…@`를 펼쳐
`.tmp`를 만들고, `file(GENERATE)`로 `$<…>`를 펼쳐 최종 파일을 만듭니다.

결과는 이렇습니다.

```python
# Generated from lit.site.cfg.py.in by CMake -- do not edit.
config.kaleidoscope_obj_root = "/…/06_llvm_tutorial/build"
config.toy_binary = "/…/06_llvm_tutorial/build/toy"
config.filecheck_binary = "/…/envs/llvm-tut/libexec/llvm/FileCheck"

lit_config.load_config(config, "/…/06_llvm_tutorial/test/lit.cfg.py")
```

마지막 줄이 사람이 쓴 설정을 불러옵니다. 즉 **lit은 `build/test/`를
가리켜야 하고**, 거기서 `test/lit.cfg.py`로 되돌아갑니다.

```bash
lit -v build/test                      # 스위트 전체
lit -v build/test/codegen/mem2reg.ks   # 파일 하나
```

소스 트리를 직접 가리키면 site 설정을 안 거치므로 `lit.cfg.py`가 참조하는
값이 아예 없어 파싱 단계에서 죽습니다.

```
$ lit -v test/
AttributeError: 'TestingConfig' object has no attribute 'kaleidoscope_obj_root'
```

이것이 README가 빌드 트리 경로를 쓰는 이유입니다.

### 6.3 도구가 없으면 건너뛴다

```cmake
find_program(LIT_BINARY NAMES lit llvm-lit)
find_program(FILECHECK_BINARY
  NAMES FileCheck
  HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_TOOLS_BINARY_DIR}/../libexec/llvm
)

if(LIT_BINARY AND FILECHECK_BINARY)
  …
  message(STATUS "IR tests enabled: ${LIT_BINARY} + ${FILECHECK_BINARY}")
else()
  message(STATUS "IR tests disabled (need lit and FileCheck)")
endif()
```

`lit`은 파이썬 패키지, `FileCheck`는 `llvm-tools`에 들어 있어 둘 다 별도
설치입니다(README의 conda 명령에 포함돼 있습니다). 없으면 IR 스위트만
빠지고 나머지 세 개는 그대로 돕니다. `HINTS`에 `libexec/llvm`이 들어간 것은
6.1과 같은 이유입니다.

---

## 7. 10개 테스트가 고정하는 것

```
test/
├── codegen/
│   ├── mem2reg.ks          alloca 없음 + PHI 3개  ← 최적화가 켜져 있다
│   ├── binop-fold.ks       uitofp/fcmp one 왕복이 접힘
│   └── user-operators.ks   연산자 = 뭉갠 이름의 함수 + 직접 호출
├── debuginfo/
│   ├── dwarf-metadata.ks   DISubprogram, DILocation, dbg_declare
│   ├── no-debug-by-default.ks   -g 없으면 메타데이터 0
│   └── no-opt.ks           -g면 alloca가 살아남고 PHI가 없다
└── driver/
    ├── toplevel-main.ks    최상위 식 → main()
    ├── one-toplevel-only.ks  최상위 식 두 개는 거부
    ├── g-requires-c.ks     -g는 -c 없이 거부
    └── bad-number.ks       잘못된 리터럴 → 진단 + .o 없음
```

`codegen/mem2reg.ks`와 `debuginfo/no-opt.ks`가 **한 쌍**이라는 점이 중요합니다.
같은 종류의 코드를 넣고 하나는 "alloca가 없어야 한다", 다른 하나는
"alloca가 있어야 한다"를 주장합니다. 차이는 `-g` 하나뿐입니다.

```
# codegen/mem2reg.ks          RUN: %toy -c %s --emit-llvm …
# CHECK-NOT:     alloca
# CHECK:         phi double

# debuginfo/no-opt.ks         RUN: %toy -c %s -g --emit-llvm …
# CHECK:         alloca double
# CHECK-NOT:     phi double
```

`-g`가 최적화를 끄는 이유([10번 문서](10-debuginfo-and-optimization.md))가
바로 이 대비입니다. mem2reg가 `alloca`를 없애면 `dbg_declare`가 가리킬 대상이
사라지기 때문에, 우리 구현은 `-g`일 때 패스를 통째로 건너뜁니다. 그 결정이
지금도 유효한지를 이 두 파일이 양쪽에서 못 박습니다.

효과는 검증됐습니다. 최적화를 항상 끄는 변이를 넣으면 `mem2reg`, `binop-fold`,
`user-operators`가 깨지고, `-g`에서도 최적화를 켜는 변이를 넣으면 `no-opt`과
`dwarf-metadata`가 깨집니다. 정확히 깨져야 할 것만 깨집니다.

---

## 8. 실패는 이렇게 보인다

`mem2reg.ks`의 `CHECK-NOT: alloca`를 `CHECK: alloca`로 뒤집어 보면
`FileCheck`가 이렇게 말합니다.

```
bad.chk:2:10: error: CHECK: expected string not found in input
# CHECK: alloca double
         ^
<stdin>:10:30: note: scanning from here
define double @fib(double %n) {
                             ^
<stdin>:20:43: note: possible intended match here
 %binop6 = call double @"binary:"(double %binop, double %addtmp)
                                          ^

Input was:
<<<<<<
           .
          10: define double @fib(double %n) {
check:2'0                                  X~~ error: no match found
          11: entry:
check:2'0     ~~~~~~~
          12:  br label %loop
check:2'0     ~~~~~~~~~~~~~~~~
```

읽을 거리가 네 개 있습니다.

- **무엇을** 못 찾았는지 (`CHECK` 줄과 그 위치)
- **어디서부터** 찾기 시작했는지 (`scanning from here` — 직전 매치 지점)
- **비슷한 것**이 어디 있었는지 (`possible intended match`)
- 입력 덤프에서 `~~~~`로 표시된 **실제로 훑은 구간**

마지막 덤프가 특히 유용합니다. `CHECK-LABEL`로 블록을 나눠 두면 이 구간이
좁아져서, 어느 함수의 어느 부분에서 어긋났는지가 바로 보입니다.
`-dump-input=always`를 주면 통과할 때도 이 덤프를 볼 수 있습니다.

---

## 9. 나머지 두 층

IR 층이 아니어서 짧게만 봅니다.

### 9.1 `lexer` — 단위 테스트

```cmake
add_executable(lexer_tests tests/LexerTests.cpp src/Lexer.cpp)
```

**LLVM 라이브러리를 하나도 링크하지 않습니다.** `Lexer`가 `SourceLocation.h`
말고는 아무것에도 의존하지 않기 때문입니다. 테스트 프레임워크도 안 씁니다 —
`CHECK` / `CHECK_EQ` 매크로 두 개면 충분하고, 그만큼 빌드 의존성이 없습니다.

애초에 단위 테스트가 가능한 것은 `Lexer`가 `getchar()` 대신
`std::istream&`을 받도록 바꾼 덕분입니다([01. Lexer](01-lexer.md) 2절).
원본 튜토리얼의 lexer는 프로그램 전체에 텍스트를 파이프로 넣고 눈으로
확인하는 것 말고는 방법이 없습니다.

### 9.2 `jit_*` — 종단간

`add_test`의 `COMMAND`는 stdin 리다이렉션도, 출력 매칭도 못 합니다.
그래서 `cmake -P`로 도는 작은 스크립트를 거칩니다.

```cmake
add_test(NAME jit_fib
  COMMAND ${CMAKE_COMMAND}
    -DTOY=$<TARGET_FILE:toy> -DINPUT=…/tests/fib.ks -DEXPECT=89
    -P …/tests/RunJit.cmake
)
```

`RunJit.cmake`는 `execute_process`로 `toy`에 파일을 물려 돌리고,
stderr에서 `Evaluated to 89.0…`을 찾습니다. 없으면 `FATAL_ERROR`로 죽고,
CMake의 종료 코드가 곧 테스트 결과가 됩니다.

---

## 10. 테스트 추가하기

C++도, CMake도, 재빌드도 필요 없습니다. 파일 하나면 됩니다.

```bash
cat > test/codegen/my-thing.ks <<'EOF'
# 무엇을 왜 확인하는지 한두 줄.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def f(x) x + x;

# CHECK-LABEL: define double @f(double %x)
# CHECK:         fadd double %x, %x
EOF

lit -v build/test/codegen/my-thing.ks
```

쓸 때 순서는 반대로 하는 게 빠릅니다.

1. 먼저 손으로 돌려서 **실제 IR을 본다**
   `./build/toy -c test/codegen/my-thing.ks --emit-llvm -o /dev/null`
2. 그 출력에서 **정말 고정하고 싶은 줄만** 골라 `CHECK:`로 옮긴다.
   전부 붙여넣으면 관계없는 변경에도 깨지는 취약한 테스트가 됩니다
3. SSA 이름·메타데이터 번호처럼 흔들리는 부분은 `{{.*}}`로 지운다
4. 일부러 틀리게 만들어 **실패하는 것까지 확인**한다

---

## 11. 정리

- 프론트엔드의 계약은 "돌아가는가"가 아니라 **"어떤 IR을 내놓는가"**.
  그래서 IR 층 테스트가 따로 있다
- `lit`은 러너, `FileCheck`는 순서를 보는 패턴 매처, `not`은 종료 코드를
  뒤집는 래퍼. `llvm/test`가 3만 개 넘게 쓰는 방식이다
- 테스트 파일 하나가 **소스이자 스크립트이자 명세**다. `#`이 Kaleidoscope의
  주석이라서 가능하다
- `CHECK-LABEL`은 블록 경계를 만들어 오염을 막고 진단을 좁힌다.
  `CHECK-NOT`은 파일 전체가 아니라 **앞뒤 매치 사이**만 본다
- 설정이 두 파일인 것은 빌드 경로 때문. `$<TARGET_FILE:toy>` 때문에
  `configure_file` + `file(GENERATE)` 2단계가 필요하다
- 테스트는 **빌드 트리**(`build/test`)에서 돈다. site 설정이 거기 있다
- conda-forge는 `FileCheck`와 `not`을 `libexec/llvm`에 둔다.
  `lit.cfg.py`가 그 디렉터리를 `PATH`에 붙여 둘 다 해결한다
- `mem2reg.ks` ↔ `no-opt.ks`가 `-g`가 최적화를 끈다는 결정을 양쪽에서 고정한다
- 테스트 추가는 파일 하나. 재컴파일 없음

---

**돌아가기**: [01. Lexer](01-lexer.md) · [07. 최적화 패스](07-passes.md) · [10. 디버그 정보와 최적화](10-debuginfo-and-optimization.md)
