# 10. 디버그 정보와 최적화 — 왜 우리는 같이 못 쓰는가

**파일**: `src/main.cpp` (`runCompile`), `src/CodeGen.cpp` (`initModule`, `codegen`)
**관련 문서**: [05. DebugInfo](05-debuginfo.md) 5절, [07. 최적화 패스](07-passes.md) 6절

[05-debuginfo](05-debuginfo.md)와 [07-passes](07-passes.md)는 각각
"`-g`면 최적화를 끈다"고 한 줄로 넘어갑니다. 이 문서는 그 한 줄을 펼칩니다.

- 실제로 `-c`와 `-c -g`의 결과물은 무엇이 다른가
- 그 제약은 **어디서 오는가** (Ch9이 게을러서가 아닙니다)
- clang과 gcc는 `-O2 -g`를 어떻게 지원하는가
- 우리가 그걸 하려면 무엇을 해야 하는가

---

## 1. 먼저 오해 하나 — `-c`는 최적화를 끄지 않는다

플래그는 하나뿐이고, `-g`에만 반응합니다.

```cpp
// main.cpp — runCompile()
CG.initModule(O.Input, TM->createDataLayout(), /*Optimize=*/!O.Debug);
```

| 실행 방식 | `FPM->run()` |
| --- | :---: |
| `./build/toy` (REPL + JIT) | ✅ (`main.cpp`에서 `true` 고정) |
| `./build/toy -c fib.ks` | ✅ |
| `./build/toy -c fib.ks -g` | ❌ |

`initModule`은 어느 경우든 패스 파이프라인을 **만듭니다**. `OptimizeFunctions`
플래그는 실행 시점에만 확인됩니다.

```cpp
// CodeGen.cpp — codegen(FunctionAST&)
if (OptimizeFunctions && FPM)
  FPM->run(*TheFunction, *FAM);
```

여기가 원본 튜토리얼과 갈리는 지점입니다. **Ch9은 파이프라인 자체를
삭제합니다** — Chapter9 `toy.cpp`의 `InitializeModule()`은 컨텍스트·모듈·빌더만
만들고,
파일 전체에 `FunctionPassManager`라는 단어가 없습니다. 즉 Ch9에서는 디버그
정보를 켜든 말든 최적화가 없습니다. 우리는 그 손실을 `-g`일 때로 한정했습니다.

> 한 가지 더: "최적화를 끈다"는 것은 **IR 수준 패스**에 한한 얘기입니다.
> `ObjectEmitter::createHostTargetMachine`은 `createTargetMachine(...)`의
> `CodeGenOptLevel` 인자를 기본값(`Default`, 대략 `-O2`)으로 두므로,
> `-g`에서도 명령어 선택·레지스터 할당·스케줄링은 그대로 최적화됩니다.
> 프론트엔드 기준의 `-O0`이지 `clang -O0`이 아닙니다.

---

## 2. 실제로 무엇이 달라지는가

`tests/fib.ks`를 두 번 컴파일해서 비교한 결과입니다.

```bash
./build/toy -c tests/fib.ks -o /tmp/plain.o --emit-llvm 2>/tmp/plain.ll
./build/toy -c tests/fib.ks -g -o /tmp/dbg.o --emit-llvm 2>/tmp/dbg.ll
```

독립적인 두 가지가 동시에 바뀝니다.

### 2.1 디버그 메타데이터가 붙는다 (`-g` 본연의 효과)

`!dbg` 부착, 함수마다 `!DISubprogram`, 인자마다 `#dbg_declare`,
`!llvm.dbg.cu`와 `Debug Info Version` 모듈 플래그, 그리고 `!DILocation` 표.
오브젝트 파일에서는 진짜 DWARF 섹션이 됩니다.

```
plain.o (1.5K):  (debug 섹션 없음)
dbg.o   (3.4K):  .debug_abbrev .debug_info .debug_str .debug_line
                 .debug_pubnames .debug_pubtypes
```

### 2.2 최적화가 멈춘다 (플래그의 부수 효과)

`@fib` 본문 기준:

| | `-c` | `-c -g` |
| --- | --- | --- |
| 명령어 수 | **12** | **33** |
| 지역 변수 | `phi` 3개 | `alloca` 5개 + 매 사용마다 load/store (17회) |
| 루프 조건 | `fcmp ult` → `br` | `fcmp ult` → `uitofp` → `fcmp one` → `br` |
| 중복 계산 | GVN이 제거 | 그대로 |

루프 조건이 가장 알기 쉬운 예입니다. Kaleidoscope에는 bool 타입이 없어서
`visitFor`가 비교 결과를 `double`로 늘린 뒤 다시 `0.0`과 비교합니다
([04-codegen](04-codegen.md) 참고). `InstCombinePass`는 이 왕복을 원래의
`fcmp` 하나로 되돌리는데, 패스를 안 돌리면 IR에 그대로 남습니다.

```llvm
; -c -g
%cmptmp   = fcmp ult double %i7, %n8
%booltmp  = uitofp i1 %cmptmp to double
%loopcond = fcmp one double %booltmp, 0.000000e+00
br i1 %loopcond, label %loop, label %afterloop

; -c
%cmptmp = fcmp ult double %i.0, %n
br i1 %cmptmp, label %loop, label %afterloop
```

---

## 3. 핵심 — `dbg_declare`는 alloca를 전제한다

위 표에서 크기 차이보다 중요한 것은 **alloca가 살아 있다**는 사실입니다.
`PromotePass`(mem2reg)는 파이프라인의 첫 패스인데
([07-passes](07-passes.md) 4절), 그게 안 돌아서 스택 슬롯이 남습니다.

우리가 변수 위치를 기술하는 방식이 딱 그것을 요구합니다.

```cpp
// DebugInfo.cpp — declareParameter()
DBuilder.insertDeclare(Alloca, D, DBuilder.createExpression(), ...);
```

```llvm
%n1 = alloca double, align 8
  #dbg_declare(ptr %n1, !16, !DIExpression(), !17)
```

`dbg_declare`의 의미는 **"변수 `n`은 이 주소에 있다, 함수 내내"** 입니다.
주소를 가리키는 표현이므로, 주소가 사라지면 말할 것이 없어집니다. mem2reg가
`%n1`을 SSA 레지스터로 승격시키면 `#dbg_declare(ptr %n1, ...)`은 존재하지 않는
슬롯을 가리키게 됩니다.

그래서 [05-debuginfo](05-debuginfo.md) 4.5절이 "변수가 메모리에 있기 때문에
어디 있는지 말할 수 있다"고 한 것입니다. **최적화를 끈 것이 우연히 도움이 된
게 아니라, `dbg_declare`만 쓰는 구현이 최적화를 끌 것을 요구합니다.**

Ch9도 같은 자리에 있습니다. 다만 Ch9은 패스를 애초에 없앴으므로 이 충돌을
만난 적이 없습니다.

---

## 4. 실제 컴파일러는 어떻게 하는가 — `dbg_declare` → `dbg_value`

clang은 `-O2 -g`를 문제없이 지원합니다. 위 충돌을 **피하지 않고 해결**합니다.

핵심은 변수 위치를 기술하는 두 번째 형식입니다.

| | 의미 | 언제 |
| --- | --- | --- |
| `#dbg_declare(ptr %slot, ...)` | 변수는 **이 주소**에 있다 (함수 내내) | 슬롯이 있을 때 |
| `#dbg_value(값, ...)` | 이 지점부터 변수는 **이 값**이다 | 슬롯이 없을 때 |

mem2reg / SROA는 alloca를 없애면서 전자를 후자로 **바꿔 씁니다**. LLVM의
`ConvertDebugDeclareToDebugValue`가 그 일을 하고, store·load·phi 각각에 대한
오버로드가 있습니다.

```
llvm/Transforms/Utils/Local.h:274-290   ConvertDebugDeclareToDebugValue(...)
```

우리 `fib.ks`와 같은 프로그램을 C로 옮겨 clang에 넣어 보면 바로 보입니다.

```
clang -O0 -g:   #dbg_declare 5개,  #dbg_value 0개
clang -O2 -g:   #dbg_declare 0개,  #dbg_value 11개
```

```llvm
; clang -O2 -g — 루프 본문
5:
  %6 = phi double [ %10, %5 ], [ 1.000000e+00, %1 ]
  %7 = phi double [ %8,  %5 ], [ 0.000000e+00, %1 ]
  %8 = phi double [ %9,  %5 ], [ 1.000000e+00, %1 ]
    #dbg_value(double %6, !19, !DIExpression(), !22)   ; i
    #dbg_value(double %7, !16, !DIExpression(), !21)   ; a
    #dbg_value(double %8, !17, !DIExpression(), !21)   ; b
  %9 = fadd double %8, %7, !dbg !27
    #dbg_value(double %9, !18, !DIExpression(), !21)   ; c
    #dbg_value(double %8, !16, !DIExpression(), !21)   ; a = b
    #dbg_value(double %9, !17, !DIExpression(), !21)   ; b = c
  %10 = fadd double %6, 1.000000e+00, !dbg !29
    #dbg_value(double %10, !19, !DIExpression(), !22)
  %11 = fcmp olt double %10, %0, !dbg !23
  br i1 %11, label %5, label %3, !dbg !25
```

**우리 `-c` 출력과 최적화 수준이 똑같습니다** — phi 3개, 접힌 비교. 거기에
"지금 이 순간 `a`·`b`·`c`가 어느 SSA 값인가"에 대한 주석이 얹혀 있을 뿐입니다.
디버거는 이걸 읽어서, 집이 없는 변수의 값을 복원합니다.

---

## 5. 그 대가 — 패스마다 지는 의무

`dbg_value`가 유지되려면 **모든 변환 패스가** 자기가 부순 것을 고쳐야 합니다.
"나중에 디버그 정보를 정리하는 패스" 같은 것은 IR 수준에 없습니다.

| 상황 | 패스가 해야 하는 일 |
| --- | --- |
| 변수가 의존하던 명령어를 삭제 | `salvageDebugInfo(I)` (`Local.h:320`) — 남은 값에서 되계산하는 `DIExpression`으로 고쳐 씀. 불가능하면 `undef` → 디버거에 `<optimized out>` |
| 명령어 둘을 병합 | `applyMergedLocation` — 공통 스코프로 내려감. 아무 줄이나 고르면 거짓말이 됨 |
| 구조체를 쪼갬 (SROA) | `DW_OP_LLVM_fragment` — "이 변수의 64~127비트가 여기 있다" |
| 인라인 | `DILocation`에 `inlinedAt` 체인 → `DW_TAG_inlined_subroutine`. 존재하지 않는 프레임이 백트레이스에 보이는 이유 |

`DIExpression`은 작은 DWARF 스택 프로그램입니다. `DW_OP_plus_uconst` 같은
연산으로 "저 레지스터 값에 8을 더한 것이 이 변수"를 표현할 수 있어서,
값이 사라져도 재구성 가능한 경우가 꽤 남습니다.

**예외 하나**: 백엔드에는 전용 패스가 있습니다. 레지스터 할당을 지나면 변수의
위치가 수시로 바뀌므로(`xmm1`에 있다가 스택에 spill 되는 식),
`LiveDebugVariables` / `LiveDebugValues`가 `DBG_VALUE`를 전파해
DWARF **location list**(`.debug_loclists`)를 만듭니다. "PC 0x10~0x2f 구간에서는
`xmm1`, 0x30부터는 `-0x18(%rbp)`" 같은 표입니다.

---

## 6. 불변식 — `-g`는 생성 코드를 바꾸면 안 된다

위 의무가 성립하려면 전제가 하나 필요합니다. **디버그 정보의 유무가
최종 기계어를 바꾸지 않아야 합니다.** 두 컴파일러 모두 이걸 기계적으로
검사합니다.

- **LLVM — `debugify`** (`llvm/Transforms/Utils/Debugify.h`)
  모든 명령어에 합성 디버그 정보를 붙이고, 파이프라인을 돌린 뒤, 무엇이
  사라졌는지 검사합니다. `opt -passes=debugify,<패스들>,check-debugify`,
  그리고 어느 패스가 범인인지 이분 탐색하는 `--debugify-each`.
- **GCC — `-fcompare-debug`**
  같은 파일을 `-g` 있이/없이 두 번 컴파일해서 오브젝트 코드가 다르면 에러.
  GCC 자기 부트스트랩에 켜져 있습니다.

이건 실제 버그 유형이었습니다. 예전 LLVM에서 디버그 정보는
`call void @llvm.dbg.value(...)` — **진짜 명령어**였습니다. 그래서 명령어 개수로
인라인 임계값을 재던 패스들이 `-g` 여부에 따라 다르게 동작했습니다. LLVM 19에서
이것들을 명령어 목록 밖의 **디버그 레코드**로 옮겼고, 그래서 문법이 `call`이
아니라 `#dbg_value`입니다. 우리 `-g` 출력에 보이는 `#dbg_declare(ptr %x1, ...)`도
같은 형식입니다 (이 저장소는 LLVM 20을 씁니다).

GCC 쪽 대응물은 `GIMPLE_DEBUG` / `DEBUG_INSN` 문(statement)이고,
`-fvar-tracking-assignments`(`-g -O`에서 기본 켜짐)가 만들고 RTL의
`var-tracking` 패스가 소비해 location list를 냅니다.

---

## 7. 그래도 실무에서는

메타데이터가 완벽해도 `-O2 -g`의 디버깅 경험은 좋지 않습니다. 실행이 줄 사이를
튀어 다니고, 변수 절반은 `<optimized out>`입니다. 그래서 중간 지점들이 있습니다.

| 옵션 | 뜻 |
| --- | --- |
| `-O0 -g` | 우리 `-c -g`와 같은 모양. clang은 여기에 `optnone` 속성까지 붙여 이후 아무도 못 건드리게 합니다 |
| `-Og` | "디버깅 친화적 최적화". GCC에서는 디버깅을 해치는 패스를 뺀 별도 레벨, clang에서는 현재 `-O1`의 별칭 |
| `-gline-tables-only` | 줄 번호 표만. 변수 위치는 포기. 크래시 심볼화·프로파일러에는 충분해서 프로덕션 빌드가 자주 씁니다. LLVM IR에서는 `emissionKind: LineTablesOnly` (우리는 `FullDebug`) |
| `-gsplit-dwarf` | 디버그 정보를 `.dwo` 파일로 분리 (링크 시간·바이너리 크기) |

---

## 8. 우리가 `-O2 -g`를 지원하려면

연습 과제로 삼을 만합니다. 필요한 변경은 셋입니다.

1. `main.cpp:235` — `/*Optimize=*/!O.Debug`를 `true`로 (또는 별도 플래그로)
2. `main.cpp:247` — `createCompileUnit(..., /*isOptimized=*/false, ...)`를 `true`로.
   디버거가 "값이 사라졌을 수 있음"을 경고하는 근거입니다
3. 디버그 정보 유지 — **아마 할 일이 없습니다.** 우리 파이프라인은
   `PromotePass`·`InstCombinePass`·`ReassociatePass`·`GVNPass`·`SimplifyCFGPass`
   전부 LLVM 기본 패스이고, 이들은 이미 4·5절의 의무를 지킵니다.
   `dbg_declare`가 `dbg_value`로 바뀌는 것도 mem2reg가 알아서 합니다

즉 프론트엔드가 `!dbg`를 성실히 붙여 두기만 하면 나머지는 LLVM이 합니다.
직접 짠 패스를 추가하는 순간부터 5절이 남의 일이 아니게 됩니다.

검증은 `llvm-dwarfdump` 출력을 같은 프로그램에 대한 clang 결과와 비교하는
것으로 합니다.

---

## 9. 직접 확인해 보기

```bash
# 1. 두 모드의 IR 비교
./build/toy -c tests/fib.ks    -o /tmp/plain.o --emit-llvm 2>/tmp/plain.ll
./build/toy -c tests/fib.ks -g -o /tmp/dbg.o   --emit-llvm 2>/tmp/dbg.ll
diff -y --width=140 /tmp/plain.ll /tmp/dbg.ll | less

# 2. alloca가 살아 있는지
grep -c alloca /tmp/plain.ll /tmp/dbg.ll        # 0 / 7 (fib 5 + binary: 2)

# 3. DWARF 섹션
readelf -S /tmp/dbg.o | grep debug

# 4. 진짜 컴파일러의 -O0 -g 대 -O2 -g
cat > /tmp/fib.c <<'EOF'
double fib(double n) {
  double a = 0, b = 1, c;
  for (double i = 1; i < n; i = i + 1) { c = a + b; a = b; b = c; }
  return b;
}
EOF
clang -O0 -g -S -emit-llvm /tmp/fib.c -o - | grep -c '#dbg_declare'   # 5
clang -O2 -g -S -emit-llvm /tmp/fib.c -o - | grep -c '#dbg_value'     # 11
clang -O2 -g -S -emit-llvm /tmp/fib.c -o - | sed -n '/define.*@fib/,/^}/p'

# 5. 최적화가 위치 정보를 얼마나 흐리는지 (location list)
clang -O2 -g -c /tmp/fib.c -o /tmp/fib-O2.o
llvm-dwarfdump --debug-info /tmp/fib-O2.o | grep -A2 DW_TAG_variable
```

---

## 10. 정리

- 우리 플래그는 `-g`에만 반응한다. **`-c`만 주면 최적화는 그대로 돈다**
- `-g`가 바꾸는 것은 둘. (1) 디버그 메타데이터가 붙는다 (2) IR 패스가 멈춘다.
  두 번째는 부수 효과다
- 패스가 멈추면 `alloca`가 살아남고, 그게 `dbg_declare`가 성립하는 조건이다.
  **우리 구현은 최적화를 끄는 것을 선택한 게 아니라 요구한다**
- 진짜 컴파일러는 mem2reg에서 `dbg_declare`를 `dbg_value`로 바꿔 이 제약을
  없앤다. 대신 **모든 패스**가 `salvageDebugInfo` 같은 의무를 진다
- 그 체계는 "`-g`가 기계어를 바꾸지 않는다"는 불변식 위에 서 있고,
  LLVM은 `debugify`, GCC는 `-fcompare-debug`로 그걸 강제한다
- 백엔드는 예외적으로 전용 패스(`LiveDebugValues`)를 두고, 결과가 DWARF
  location list다
- 우리 toy에 `-O2 -g`를 붙이는 것은 플래그 두 개 문제다. 표준 패스만 쓰는 한
  디버그 정보 유지는 LLVM이 해 준다

---

**돌아가기**: [05. DebugInfo](05-debuginfo.md) · [07. 최적화 패스](07-passes.md)
