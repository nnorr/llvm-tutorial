# 08. JIT 컴파일러 구조

**파일**: `include/KaleidoscopeJIT.h` (upstream에서 그대로 가져옴), `src/main.cpp`
**원본 튜토리얼**: Chapter 4 — *Adding JIT and Optimizer Support*, 그리고 별도 시리즈 *Building a JIT*

---

## 1. JIT이란

**A**head-**O**f-**T**ime 컴파일: 미리 기계어를 만들어 파일로 저장하고, 나중에 실행.

**J**ust-**I**n-**T**ime 컴파일: **실행 중에** 기계어를 만들어 **메모리에 두고
바로 호출**. 파일을 거치지 않습니다.

```
AOT:  소스 → IR → .o 파일 → 링크 → 실행 파일 → (나중에) 실행
JIT:  소스 → IR → 메모리 상의 기계어 → 함수 포인터로 즉시 호출
```

우리 REPL이 이렇게 동작합니다.

```
$ ./build/toy
ready> def f(x) x*2;
ready> f(21);
Evaluated to 42.000000        ← 방금 기계어로 컴파일해서 실제로 호출한 결과
```

`42`는 인터프리터가 계산한 값이 아닙니다. **네이티브 x86-64 코드가 만들어져
CPU에서 실행된** 결과입니다.

---

## 2. ORC — LLVM의 JIT API

**O**n-**R**equest **C**ompilation, 버전 2 (ORCv2). 계층(layer) 구조입니다.

```
   ThreadSafeModule (IR)
        │
        ▼
   ┌─────────────────────┐
   │  IRCompileLayer     │   IR → 오브젝트 코드 (메모리 상의 .o)
   └─────────────────────┘
        │
        ▼
   ┌─────────────────────┐
   │ RTDyldObjectLinking │   오브젝트 → 실행 가능한 메모리에 배치 + 재배치
   │        Layer        │
   └─────────────────────┘
        │
        ▼
   실행 가능한 기계어 (주소를 얻어 호출 가능)
```

각 층은 "위에서 받아 처리하고 아래로 넘기는" 구조입니다. 층을 갈아끼우면
동작을 바꿀 수 있습니다 (예: 지연 컴파일 층 추가).

---

## 3. `KaleidoscopeJIT` 멤버

```cpp
class KaleidoscopeJIT {
private:
  std::unique_ptr<ExecutionSession> ES;

  DataLayout DL;
  MangleAndInterner Mangle;

  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer CompileLayer;

  JITDylib &MainJD;
```

| 멤버 | 역할 |
| --- | --- |
| `ES` | **ExecutionSession** — JIT 전체의 중심. 심볼 테이블, 조회, 스레드 관리 |
| `DL` | **DataLayout** — 타입 크기·정렬·심볼 접두사 규칙 |
| `Mangle` | 이름을 플랫폼 규칙에 맞게 변형 + 내부 문자열로 등록 |
| `ObjectLayer` | 오브젝트 코드를 메모리에 올리고 주소를 확정 |
| `CompileLayer` | IR을 오브젝트 코드로 컴파일 |
| `MainJD` | **JITDylib** — 심볼이 사는 공간. 동적 라이브러리 하나에 해당 |

### `JITDylib`가 핵심 개념

**JIT 안의 가상 동적 라이브러리**입니다. 실제 `.so` 파일처럼, 심볼 이름 →
주소의 매핑을 갖습니다. 여기에 모듈을 추가하고, 여기서 심볼을 찾습니다.

`MainJD`가 참조(`&`)인 것에 주의하세요. **소유자는 `ES`입니다.**
`ES->createBareJITDylib("<main>")`가 만들어 `ES` 안에 보관하고 참조를 돌려줍니다.

---

## 4. `Create()` — 만드는 과정

```cpp
static Expected<std::unique_ptr<KaleidoscopeJIT>> Create() {
  auto EPC = SelfExecutorProcessControl::Create();
  if (!EPC)
    return EPC.takeError();

  auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));

  JITTargetMachineBuilder JTMB(
      ES->getExecutorProcessControl().getTargetTriple());

  auto DL = JTMB.getDefaultDataLayoutForTarget();
  if (!DL)
    return DL.takeError();

  return std::make_unique<KaleidoscopeJIT>(std::move(ES), std::move(JTMB),
                                           std::move(*DL));
}
```

### `Expected<T>` — 예외 없는 오류 처리

LLVM은 보통 C++ 예외를 끄고 빌드됩니다. 대신 **`Expected<T>`** 를 씁니다.
"성공하면 `T`, 실패하면 오류"를 담는 상자입니다.

```cpp
auto EPC = SelfExecutorProcessControl::Create();
if (!EPC)                       // bool 변환: 성공이면 true
  return EPC.takeError();       // 실패면 오류를 꺼내 전달
... *EPC ...                    // 성공이면 * 로 값을 꺼냄
```

**오류를 확인하지 않고 버리면 런타임에 프로그램이 죽습니다.** 실수로 무시하는
것을 막기 위한 설계입니다.

`main.cpp`의 `ExitOnError ExitOnErr;` 는 "오류면 메시지 찍고 종료"하는 도우미로,
예제 코드에서 오류 처리를 짧게 쓰기 위한 것입니다.

```cpp
auto TheJIT = ExitOnErr(orc::KaleidoscopeJIT::Create());
//            ^^^^^^^^^ Expected를 풀어 값만 꺼냄. 실패면 종료
```

### 세 단계

1. **`SelfExecutorProcessControl`** — "컴파일한 코드를 **어디서** 실행할지".
   `Self` = 지금 이 프로세스 안에서. (원격 프로세스에서 실행하는 변형도 있습니다.)
2. **`JITTargetMachineBuilder`** — 현재 머신의 타깃 트리플로 코드 생성기를 준비
3. **`DataLayout`** — 타깃에서 `double`이 몇 바이트인지 등의 규칙을 얻음

3번이 `CodeGen`으로 전달되는 값입니다.

```cpp
CG.initModule("KaleidoscopeJIT", TheJIT->getDataLayout(), true);
```

**`CodeGen`이 JIT에게서 필요로 하는 것은 이 `DataLayout` 하나뿐입니다.**
그래서 `CodeGen`은 JIT을 몰라도 되고, 오브젝트 파일 모드에서는 대신
`TargetMachine`의 `DataLayout`을 받습니다
([06-backend-and-driver](06-backend-and-driver.md) 1절).

---

## 5. 생성자 — 층 조립

```cpp
KaleidoscopeJIT(std::unique_ptr<ExecutionSession> ES,
                JITTargetMachineBuilder JTMB, DataLayout DL)
    : ES(std::move(ES)), DL(std::move(DL)), Mangle(*this->ES, this->DL),
      ObjectLayer(*this->ES,
                  []() { return std::make_unique<SectionMemoryManager>(); }),
      CompileLayer(*this->ES, ObjectLayer,
                   std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
      MainJD(this->ES->createBareJITDylib("<main>")) {
  MainJD.addGenerator(
      cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
          DL.getGlobalPrefix())));
  ...
}
```

### `this->` 가 붙은 이유

매개변수 이름 `ES`가 멤버 이름 `ES`를 **가립니다(shadowing)**. 초기화 리스트에서
`ES(std::move(ES))` 는 "멤버 `ES`를 매개변수 `ES`로 초기화"인데, 그 **다음**
줄부터 `ES`라고 쓰면 여전히 매개변수를 가리킵니다. 매개변수는 이미
`std::move`로 비워졌으므로 쓰면 안 됩니다.

그래서 이미 초기화된 멤버를 쓸 때는 `this->ES`로 명시합니다. C++ 초기화 순서
규칙에서 자주 나오는 함정입니다.

### 람다(lambda)

```cpp
[]() { return std::make_unique<SectionMemoryManager>(); }
```

**이름 없는 함수**입니다. `[]`는 캡처 목록(바깥 변수를 가져다 쓸 것 목록,
여기서는 비어 있음), `()`는 매개변수, `{}`는 본문입니다.

`ObjectLayer`는 메모리가 필요할 때마다 이 함수를 불러 새 관리자를 얻습니다.
"어떻게 메모리를 잡을지"를 주입하는 것입니다.

`SectionMemoryManager`는 **실행 가능한 메모리**를 할당합니다. 보통 힙 메모리는
실행 권한이 없어서 그대로는 코드를 담을 수 없습니다.

### `ConcurrentIRCompiler`

`CompileLayer`에 주입되는 실제 컴파일러입니다. IR을 받아 오브젝트 코드를
만듭니다. `Concurrent`는 여러 스레드에서 동시에 안전하다는 뜻입니다.

### `DynamicLibrarySearchGenerator` — `putchard`가 발견되는 곳

```cpp
MainJD.addGenerator(
    cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
        DL.getGlobalPrefix())));
```

**제너레이터**는 "JITDylib에서 심볼을 못 찾았을 때 대신 찾아 주는 것"입니다.
이것은 **현재 프로세스의 심볼 테이블**을 뒤집니다.

그래서 이렇게 동작합니다.

```
extern putchard(c);
putchard(72);
```

1. JIT이 `putchard`를 `MainJD`에서 찾음 → 없음
2. 제너레이터가 현재 프로세스에서 찾음 → `main.cpp`가 정의한 `putchard` 발견
3. 그 주소로 연결

**이것이 성립하려면 두 가지가 필요합니다** ([06-backend-and-driver](06-backend-and-driver.md) 3.2절):

- `extern "C"` — C++ 이름 변형을 꺼서 심볼 이름이 그대로 `putchard`
- `ENABLE_EXPORTS` (= `-rdynamic`) — 실행 파일의 심볼을 동적 심볼 테이블에 노출

둘 중 하나라도 빠지면 링크는 되는데 실행 중에 심볼을 못 찾습니다.

`cantFail(...)`은 "실패할 리 없다"고 단언하는 도우미입니다. 실패하면 즉시 종료합니다.

---

## 6. `addModule` — 모듈 추가

```cpp
Error addModule(ThreadSafeModule TSM, ResourceTrackerSP RT = nullptr) {
  if (!RT)
    RT = MainJD.getDefaultResourceTracker();
  return CompileLayer.add(RT, std::move(TSM));
}
```

### `ThreadSafeModule`

`Module` + `LLVMContext`를 **함께 묶은 것**입니다. `Module`은 자기 `Context`에
의존하므로 둘의 수명이 엮여 있어야 하고, 스레드 안전을 위해서도 함께 관리됩니다.

`main.cpp`에서 이렇게 만듭니다.

```cpp
auto TSM = orc::ThreadSafeModule(CG.takeModule(), CG.takeContext());
```

`CodeGen`이 `takeModule()`/`takeContext()`로 **소유권을 넘기는** 이유입니다.
넘긴 뒤에는 `CG.initModule(...)`로 새 모듈을 열어야 합니다.

### 지연 컴파일 (lazy)

**중요**: `CompileLayer.add()`는 **컴파일하지 않습니다.** "이 모듈에 이런
심볼들이 있다"고 등록만 합니다. 실제 컴파일은 **누군가 그 심볼을 찾을 때**
일어납니다. ORC의 이름 **O**n-**R**equest **C**ompilation이 여기서 나옵니다.

그래서 REPL에서 함수를 100개 정의해도, 호출하지 않은 함수는 기계어로
변환되지 않습니다.

---

## 7. `lookup` — 심볼 찾기와 이름 변형

```cpp
Expected<ExecutorSymbolDef> lookup(StringRef Name) {
  return ES->lookup({&MainJD}, Mangle(Name.str()));
}
```

`Mangle(...)`이 **플랫폼 심볼 규칙**을 적용합니다. 예를 들어 macOS는 모든 C
심볼 앞에 `_`가 붙습니다. `DL.getGlobalPrefix()`가 그 접두사를 알려주고,
`MangleAndInterner`가 적용합니다.

리눅스에서는 접두사가 없어 이름이 그대로지만, **코드를 플랫폼 독립적으로
유지하기 위해** 항상 거칩니다.

`{&MainJD}`는 "이 JITDylib들에서 찾아라"는 검색 순서 목록입니다.

반환된 `ExecutorSymbolDef`에서 주소를 꺼내 함수 포인터로 바꿉니다.

```cpp
auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
double (*FP)() = ExprSymbol.toPtr<double (*)()>();
fprintf(stderr, "Evaluated to %f\n", FP());
```

`double (*FP)()` 는 **함수 포인터** 선언입니다. 읽는 순서:

```
double (*FP)()
       ^^^^      FP는 포인터
           ^^    함수를 가리키는 (인자 없음)
^^^^^^           double을 반환하는
```

`toPtr<T>()`가 JIT이 알려준 주소를 그 타입으로 해석합니다. `FP()`를 부르는 순간
**방금 생성된 기계어로 점프**합니다.

여기서 타입을 틀리면 (예: 인자가 있는데 없다고 선언) 정의되지 않은 동작입니다.
Kaleidoscope는 모든 함수가 `double(...)` 이라 안전합니다.

---

## 8. `ResourceTracker` — 모듈 수명 관리

```cpp
Error addModule(ThreadSafeModule TSM, ResourceTrackerSP RT = nullptr)
```

두 번째 인자가 **자원 추적자**입니다. 그 모듈이 사용한 메모리에 붙는 꼬리표로,
`RT->remove()`로 한 번에 회수할 수 있습니다.

`main.cpp`의 사용이 두 가지로 갈립니다.

```cpp
// 함수 정의 — 추적자 없음 = 영구
ExitOnErr(TheJIT->addModule(std::move(TSM)));

// 최상위 식 — 추적자 있음 = 평가 후 제거
auto RT = TheJIT->getMainJITDylib().createResourceTracker();
ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
...
ExitOnErr(RT->remove());
```

REPL에서 `1+2;`, `3*4;` … 를 계속 입력하면 매번 `__anon_expr` 함수가 생깁니다.
치우지 않으면 메모리가 쌓이고, 같은 이름이 중복 정의됩니다. 추적자가 이를
해결합니다.

**반대로 함수 정의는 남아 있어야 합니다.** 나중에 호출해야 하니까요.
이 구분을 틀려서 실제로 버그를 냈습니다 —
[06-backend-and-driver](06-backend-and-driver.md) 3.5절에 증상까지 기록해 뒀습니다.

`ResourceTrackerSP`의 `SP`는 **shared pointer**입니다. `unique_ptr`과 달리
여러 곳이 함께 소유할 수 있고, 마지막 소유자가 사라질 때 해제됩니다.

---

## 9. 전체 흐름 (최상위 식 하나)

```
사용자 입력:  f(21);
     │
     ▼
Parser        → FunctionAST (__anon_expr 로 감싼 것)
     │
     ▼
CodeGen       → Module 안에 define double @__anon_expr()
     │           (여기서 최적화 패스도 실행됨)
     ▼
CG.takeModule() + takeContext()   ← 소유권 이전
     │
     ▼
ThreadSafeModule
     │
     ▼
RT = createResourceTracker()      ← 나중에 지우려고
TheJIT->addModule(TSM, RT)        ← 아직 컴파일 안 됨 (등록만)
     │
     ▼
CG.initModule(...)                ← 새 모듈 열기
     │
     ▼
TheJIT->lookup("__anon_expr")     ← ★ 여기서 실제 컴파일 발생
     │                                IRCompileLayer → ObjectLayer → 주소
     ▼
FP = 주소를 double(*)() 로 해석
FP()                              ← 네이티브 코드 실행
     │
     ▼
RT->remove()                      ← 익명 함수 정리
```

★ 표시가 지연 컴파일이 일어나는 지점입니다. `addModule`이 아니라 `lookup`입니다.

---

## 10. 직접 확인해 보기

```bash
conda activate llvm-tut && cmake --build build

# JIT이 실제로 네이티브 코드를 실행하는지
echo 'def f(x) x*2; f(21);' | ./build/toy

# 프로세스 심볼 해석 (putchard는 main.cpp의 C++ 함수)
echo 'extern putchard(c); putchard(72); putchard(73);' | ./build/toy

# printd 도 있음
echo 'extern printd(x); printd(3.14);' | ./build/toy

# libm 함수도 프로세스에 있으므로 찾아짐
echo 'extern cos(x); cos(0);' | ./build/toy

# 정의가 영구적인지 (ResourceTracker 없음) 확인
printf 'def sq(x) x*x;\nsq(4);\nsq(5);\nsq(6);\n' | ./build/toy

# 실행 파일에 putchard 심볼이 노출돼 있는지 (ENABLE_EXPORTS 효과)
nm -D build/toy | grep -E 'putchard|printd'
```

마지막 명령이 비어 있으면 `extern putchard` 가 실행 중에 실패합니다.
`-rdynamic` 없이 빌드했을 때 정확히 그렇게 됩니다.

---

## 11. 이 프로젝트가 쓰지 않는 것

`KaleidoscopeJIT`은 튜토리얼용 최소 구현입니다. ORC가 제공하지만 여기 없는 것들:

| 기능 | 설명 |
| --- | --- |
| 지연 함수 컴파일 | 호출되는 순간 함수 단위로 컴파일 (`LazyCallThroughManager`) |
| 원격 실행 | 다른 프로세스/기기에서 코드 실행 |
| 재최적화 | 자주 쓰이는 함수를 더 세게 최적화해 교체 |
| 여러 JITDylib | 심볼 격리, 버전 분리 |

원본 튜토리얼에 **Building a JIT** 이라는 별도 4챕터 시리즈가 있고
(`llvm/examples/Kaleidoscope/BuildingAJIT/`), 위 주제들을 다룹니다.

---

## 12. 정리

- JIT = 실행 중에 기계어를 만들어 메모리에서 바로 호출
- ORCv2는 **층 구조**: `IRCompileLayer`(IR→오브젝트) → `ObjectLayer`(메모리 배치)
- `ExecutionSession`이 중심, `JITDylib`이 심볼 공간(가상 .so)
- LLVM은 예외 대신 **`Expected<T>`/`Error`** 로 실패를 알린다. 무시하면 죽는다
- `CodeGen`이 JIT에서 필요로 하는 것은 **`DataLayout` 하나뿐**
- **지연 컴파일**: `addModule`은 등록만, 실제 컴파일은 `lookup` 시점
- `DynamicLibrarySearchGenerator`가 프로세스 심볼을 뒤져 `putchard`를 찾는다.
  `extern "C"` + `-rdynamic` 둘 다 필요
- `ResourceTracker`로 임시 모듈만 골라 회수한다.
  **정의는 영구, 최상위 식은 임시**
- `double (*FP)()` 로 주소를 해석해 호출하면 네이티브 코드가 실행된다

---

**문서 끝.** 설계 배경 전체는 [`../ARCHITECTURE.md`](../ARCHITECTURE.md) 참고.
