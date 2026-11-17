# mini-dbms

C로 밑바닥부터 만든 미니 DBMS 데모입니다. 외부 라이브러리 없이 libc와 pthread만 사용합니다.

| Phase | 결과물 | 핵심 파일 |
|---|---|---|
| 1. SQL 처리기 | CLI로 `INSERT`/`SELECT`를 실행하는 파일 기반 처리기 | `lexer.c` `parser.c` `executor.c` `table.c` `cli.c` |
| 2. B+ 트리 인덱스 | id 자동 부여 + B+ 트리 인덱스 + 100만 건 성능 비교 | `bptree.c` `tools/bench.c` |
| 3. API 서버 | Phase 1+2 엔진을 감싼 HTTP 서버 + 스레드 풀 | `threadpool.c` `server.c` |

---

## 빠른 시작

리눅스(또는 WSL, Docker)에서 gcc, make, python3가 필요합니다.

```bash
docker build -t mini-dbms-dev .
docker run --rm -it -v "$PWD":/work -w /work -p 8080:8080 mini-dbms-dev bash
```

```bash
make                              # build/mdb, build/mdb_server, build/bench, build/unit_tests
./build/mdb examples/demo.sql     # Phase 1: SQL 파일 실행
./build/mdb                       # Phase 1: 대화형 REPL
make bench                        # Phase 2: 100만 건 벤치마크
./build/mdb_server -p 8080        # Phase 3: API 서버
make test                         # 단위 테스트 + API 기능 테스트
make asan && ./build/unit_tests   # 메모리 오류 검사 빌드
```

---

## 전체 구조

```
                 ┌──────────── 내부 DB 엔진 (Phase 1 + 2) ────────────┐
 cli.c ─┐        │                                                     │
 bench.c┼─ SQL ─▶│ Lexer ──▶ Parser ──▶ Executor ──▶ Table (Storage)   │──▶ data/users.tbl
 server.c┘ 텍스트 │ 토큰     Statement   의미 검사      ├ 파일 append/scan │
   ▲              │                     실행 계획      └ B+ 트리 (id→offset)
   │              └─────────────────────────────────────────────────────┘
 HTTP ◀── 스레드 풀 (워커 N개) ◀── accept 스레드 ◀── 클라이언트 (Phase 3)
```

세 실행 파일(`mdb`, `bench`, `mdb_server`)은 모두 **같은 함수 하나** `db_execute_script(db, sql, callback)`로 엔진을 호출합니다. Phase 1의 SQL 처리기가 그대로 Phase 2와 3의 내부 엔진이 되는 구조입니다.

### 공부 순서 추천
1. `src/lexer.c`: 커서를 옮기며 첫 글자로 토큰 종류를 결정
2. `src/parser.c`: `expect`/`accept`와 문법 규칙당 함수 하나 (재귀 하강)
3. `src/table.c`: 행 직렬화, append, 크래시 복구
4. `src/executor.c`: 의미 검사, 실행 계획 선택 (인덱스 vs 풀 스캔), 락
5. `src/bptree.c`: 삽입과 분할(split), 리프 연결 리스트
6. `src/threadpool.c` → `src/server.c`: 작업 큐, 조건 변수, HTTP 처리

---

## 지원하는 SQL

```sql
INSERT INTO users VALUES ('kim', 20);                          -- id 는 자동 부여
INSERT INTO users (age, name) VALUES (25, 'lee'), (30, 'park'); -- 컬럼 순서 지정, 여러 행
SELECT * FROM users;
SELECT name, age FROM users WHERE id = 3;                       -- B+ 트리
SELECT * FROM users WHERE id >= 100 LIMIT 10;                   -- B+ 트리 범위 검색
SELECT id FROM users WHERE name = 'O''Brien';                   -- 풀 스캔
```

- 비교 연산자: `=` `!=` `<>` `<` `<=` `>` `>=`
- 타입: `INT` (64비트 정수), `TEXT` (최대 255바이트, 줄바꿈 불가)
- 키워드와 테이블/컬럼 이름은 대소문자를 구분하지 않습니다. `--` 주석을 지원합니다.
- `CREATE TABLE`은 과제 조건에 따라 구현하지 않았습니다. `data/<table>.schema` 파일로 정의합니다.

```
# data/users.schema   (첫 컬럼은 반드시 id INT)
id INT
name TEXT
age INT
```

---

## Phase 1. SQL 처리기

### 처리 흐름: 입력 → 파싱 → 실행 → 저장

| 단계 | 입력 → 출력 | 하는 일 | 잡는 에러 |
|---|---|---|---|
| Lexer | 문자열 → Token | 공백/주석 제거, 키워드 대문자화, `'...'` 처리 | 닫히지 않은 문자열, 이상한 문자 |
| Parser | Token → Statement | 문법 검사, 구조체 생성 | `INTO` 누락, `;` 누락 |
| Executor | Statement → Result | 스키마 대조, 실행 계획 선택, 락 | 없는 테이블/컬럼, 값 개수, 타입 |
| Table | 행 ↔ 파일 | 직렬화, append, 스캔, 복구 | 파일 손상, 쓰기 실패 |

- Parser는 Lexer에게 토큰을 **필요할 때 하나씩** 받습니다(스트리밍). 100만 줄짜리 SQL 파일도 토큰 전체를 메모리에 올리지 않습니다.
- 한 문장에서 문법 에러가 나면 다음 `;`까지 건너뛰고 **다음 문장은 계속 실행**합니다(에러 복구).
- 모든 에러에 줄 번호가 붙습니다: `ERROR line 20, col 34: unterminated string literal`

### 저장 포맷

```
data/users.tbl
1,"민수",25
2,"O'Brien, Jr",22          ← TEXT 는 항상 "..." 로 감싸고 내부의 " 는 "" 로
@B 2                       ← 여러 행 INSERT 묶음 시작 표시 (다음 2줄이 한 묶음)
3,"kim, lee",41
4,"park",30
```

| 결정 | 선택 | 이유 |
|---|---|---|
| 파일 구조 | 테이블당 `.schema` + `.tbl` | 단순하고 테이블끼리 독립적 |
| 행 형식 | CSV 형태의 텍스트, 가변 길이 | 사람이 읽고 디버깅하기 쉬움, 공간 낭비 없음 |
| 쓰기 | 항상 파일 끝에 append | 기존 데이터를 건드리지 않아 안전 |
| 위치 찾기 | B+ 트리에 **바이트 offset** 저장 | 가변 길이라 n번째 행 위치를 계산할 수 없기 때문 |

### 원자성과 크래시 복구

- **실행 중 실패**: 여러 행 INSERT는 모든 행을 먼저 검증한 뒤에만 씁니다. 3번째 행 타입이 틀리면 1, 2번째 행도 들어가지 않습니다.
- **쓰기 방식**: 묶음 전체를 메모리 버퍼에 직렬화한 뒤 `fwrite` 한 번으로 기록하고 `fflush`합니다. 쓰기에 실패하면 이전 크기로 `truncate`해서 되돌립니다.
- **크래시 복구**: 테이블을 열 때 파일을 끝까지 읽으면서 다음 두 경우를 찾아 그 시작 위치에서 잘라냅니다.
  1. `\n`으로 끝나지 않은 마지막 줄 (한 행을 쓰다가 중단)
  2. `@B n` 뒤에 n줄이 다 오지 않은 묶음 (여러 행을 쓰다가 중단)
  → 여러 행 INSERT는 크래시가 나도 **전부 아니면 전무**입니다.
- 손상된 행(파싱 불가, 중복 id)이 파일 중간에 있으면 조용히 넘어가지 않고 **열기를 거부**합니다.

---

## Phase 2. B+ 트리 인덱스

- **key = id, value = 파일 안의 바이트 offset.** 행 데이터 자체는 파일에 있고, 트리는 "어디 있는지"만 압니다.
- order 64 (노드당 키 최대 64개). 100만 건에서 높이 4입니다.
- **id 자동 부여**: `next_id`는 열 때 파일에서 찾은 최대 id + 1입니다. 실패한 INSERT는 id를 소비하지 않습니다.
- **메모리 기반**이므로 테이블을 열 때마다 파일을 훑어서 트리를 다시 만듭니다. 순서는 **복구(잘라내기) → 인덱스 구축**입니다.
- 리프 노드끼리 `next`로 연결되어 있어 `WHERE id >= 100` 같은 범위 검색은 시작 리프를 찾은 뒤 옆으로 훑습니다.

### 실행 계획 선택 (`executor.c`)

| WHERE 조건 | 계획 | 읽는 행 수 |
|---|---|---|
| `id = k` | INDEX LOOKUP | 1 |
| `id <, <=, >, >= k`, 예상 결과 ≤ 전체의 14% | INDEX RANGE SCAN | 결과 행 수만큼 |
| `id <, <=, >, >= k`, 예상 결과 > 전체의 14% | FULL SCAN (선형 탐색) | 전체 |
| `id != k` 또는 다른 컬럼 | FULL SCAN (선형 탐색) | 전체 |

### 범위 조건의 옵티마이저: 결과가 많으면 풀 스캔

인덱스 범위 검색은 행마다 `fseek` 후 한 줄을 읽습니다(한 행에 약 0.6 µs). 풀 스캔은 파일을 처음부터 순서대로 읽습니다(한 행에 약 0.09 µs). 그래서 결과가 전체의 일정 비율을 넘으면 **풀 스캔이 더 빠릅니다**.

- **예상 결과 계산**: id는 1씩 늘어나고 빈틈이 없습니다(append-only, 삭제 없음). 그래서 데이터를 읽지 않고 범위만으로 결과 행 수를 계산합니다. `LIMIT`이 있으면 인덱스는 그만큼만 읽고 멈추므로, 예상 결과를 `LIMIT` 값으로 줄여서 봅니다.
- **기준값 14%** (`index_range_max_ratio`, `executor.c`): `./build/bench`의 [5] 항목으로 측정해서 정했습니다.
- 실행 계획에 판단 근거가 함께 나옵니다. 예: `FULL SCAN + filter (id >= 1) [optimizer: est. 100.0% of rows > 14%]`

`./build/bench -n 1000000` [5] 항목 결과 (같은 행 집합을 `WHERE id > X`와 인덱스 없는 컬럼 `WHERE k > X`로 비교, 3회 중 최솟값):

| 결과 비율 | 결과 행 수 | 인덱스 | 풀 스캔 | 옵티마이저 선택 |
|---|---|---|---|---|
| 1% | 10,000 | 6.3 ms | 89.4 ms | 인덱스 |
| 10% | 100,000 | 63.2 ms | 92.1 ms | 인덱스 |
| 14% | 140,000 | 90.8 ms | 100.9 ms | 인덱스 |
| 16% | 160,000 | 105.4 ms | 104.4 ms | 풀 스캔 |
| 20% | 200,000 | 126.8 ms | 96.6 ms | 풀 스캔 |
| 50% | 500,000 | 348.0 ms | 137.9 ms | 풀 스캔 |
| 100% | 1,000,000 | 733.2 ms | 216.1 ms | 풀 스캔 (3.4배 빠름) |

역전 지점은 측정할 때마다 14~16% 사이에서 조금씩 달라집니다. 10만 건에서도 13~14% 근처였습니다. 다만 하드웨어나 행 크기가 바뀌면 이 지점도 바뀌므로, 환경이 달라지면 [5] 항목을 다시 재서 기준값을 조정해야 합니다.

### 벤치마크 결과

`./build/bench -n 1000000 -q 10` (Docker, Ubuntu 24.04, gcc -O2, 컨테이너 내부 디스크). 삽입도 조회도 모두 **SQL 문자열을 Phase 1 처리기로 실행**한 결과입니다.

| 항목 | 결과 |
|---|---|
| INSERT 1,000,000행 (문장당 1,000행) | 1,239 ms (약 807,000 rows/sec), 데이터 파일 47.4 MB |
| 재시작: 파일 스캔 + B+ 트리 재구축 | 110 ms, 높이 4, 노드 32,224개, 불변식 검사 통과 |

| 쿼리 (무작위 id 10개, 같은 행을 두 방법으로 찾음) | 평균 | 읽은 행 수 |
|---|---|---|
| `WHERE id = k` (B+ 트리) | **0.043 ms** | 1 |
| `WHERE name = 'user_k'` (풀 스캔) | **105.2 ms** | 1,000,000 |
| → 인덱스가 약 **2,400배** 빠름 | | |
| `WHERE id >= 999001` (범위, 1,000행) | 0.76 ms | 1,000 |
| `WHERE age = 30` (풀 스캔, 16,667행) | 103.6 ms | 1,000,000 |

해석
- 인덱스 조회 시간은 데이터 양과 거의 무관합니다. 트리 높이(4)만큼 비교한 뒤 `fseek` 한 번으로 한 줄을 읽습니다. 반면 풀 스캔은 행 수에 비례합니다.
- 풀 스캔 시간의 대부분은 100만 줄을 읽고 파싱하는 비용입니다. 텍스트 포맷을 선택한 대가이고, 고정 길이 바이너리였다면 스캔이 더 빨랐을 것입니다.
- 범위 검색은 시작점만 트리로 찾고 리프 연결 리스트를 따라가므로 결과 행 수에 비례합니다.

```bash
python3 tools/gen_sql.py 1000000 > big.sql   # SQL 파일로 만들어서
./build/mdb -q big.sql                        # CLI 로 넣는 방법도 있습니다
```

---

## Phase 3. API 서버

```bash
./build/mdb_server -d data -p 8080 -t 8 -q 64
```

| 메서드 | 경로 | 설명 |
|---|---|---|
| `POST` | `/query` | 본문 = SQL 텍스트 (여러 문장 가능). 전부 성공하면 200, 하나라도 실패하면 400 |
| `GET` | `/health` | `{"ok":true}` |
| `GET` | `/stats` | 스레드 풀 상태(대기/처리 중/완료/거절 수), 테이블별 행 수와 인덱스 높이 |

```bash
curl -s -X POST localhost:8080/query --data "INSERT INTO users VALUES ('kim', 20);"
curl -s -X POST localhost:8080/query --data "SELECT * FROM users WHERE id = 1;"
```

```json
{"ok":true,"statements":1,"errors":0,"results":[{"ok":true,"type":"select","table":"users",
 "columns":["id","name","age"],"rows":[[1,"kim",20]],"row_count":1,
 "plan":"INDEX LOOKUP on id (B+ tree, id = 1)","index_used":true,"rows_examined":1,"elapsed_ms":0.012}]}
```

### 스레드 풀 구조

```
main 스레드: accept() ──▶ tp_submit(fd)
                              │ 큐가 가득 차면 → 즉시 503 (backpressure)
                              ▼
               [ 원형 큐 (크기 제한) ]  ← mutex 1개 + 조건 변수 1개
                 │      │       │
              워커0   워커1 ... 워커N-1   ← 요청 읽기 → SQL 실행 → JSON 응답
```

- 요청마다 스레드를 만들지 않고 미리 만든 N개를 재사용합니다.
- 큐가 비면 워커는 `pthread_cond_wait`로 잠듭니다. 깨어난 뒤 조건을 `while`로 다시 확인합니다(spurious wakeup 대비).
- 응답 헤더 `X-Worker`로 어느 워커가 처리했는지 확인할 수 있습니다.
- 소켓 타임아웃 5초: 데이터를 보내지 않는 느린 클라이언트가 워커를 무한정 붙잡지 못하게 합니다.
- `Ctrl+C`(SIGINT) → 새 연결 받기를 멈추고, 큐에 남은 요청을 다 처리한 뒤 종료합니다(graceful shutdown).

### 동시성 설계

| 공유 자원 | 보호 방법 |
|---|---|
| Lexer, Parser, Result | 요청마다 새로 만듦 → 공유 없음 |
| 테이블 목록 | 시작할 때 한 번 만들고 바뀌지 않음 → 락 불필요 |
| 테이블 데이터 (파일 + B+ 트리 + next_id) | 테이블별 **rwlock**: SELECT는 읽기 락(여럿이 동시에), INSERT는 쓰기 락(혼자) |
| 스레드 풀 큐 | mutex + 조건 변수 |

- **id 부여와 파일 기록이 같은 쓰기 락 안에서** 일어나므로 id는 중복도 빈틈도 없고, 파일 순서와 id 순서가 같습니다.
- 여러 행 INSERT는 쓰기 락을 잡은 채 한 번에 기록되므로, 동시에 실행되는 SELECT가 묶음의 일부만 보는 일이 없습니다(격리성).
- **writer starvation 방지**: glibc의 rwlock 기본값은 읽기 우선입니다. 그래서 SELECT가 끊임없이 들어오면 INSERT가 영원히 대기할 수 있습니다. 실제로 동시성 테스트에서 이 문제로 테스트가 멈췄고, `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`(쓰기 우선)로 바꿔 해결했습니다.
- **RST 문제**: 요청 본문을 다 읽지 않은 채 `close()`하면 커널이 RST를 보내 클라이언트가 503/413 응답을 못 받습니다. 그래서 `shutdown(SHUT_WR)` 후 남은 입력을 비우고 닫습니다. 이것도 테스트에서 발견해 고쳤습니다.

---

## 테스트

```bash
make test
```

| 종류 | 파일 | 내용 |
|---|---|---|
| 단위: Lexer | `tests/test_lexer.c` | 대소문자, `selected` 같은 키워드 접두사, 문자열 안의 쉼표/공백, `''` 이스케이프, 한글, 미종료 문자열, 연산자, 주석, 줄/열 번호 |
| 단위: Parser | `tests/test_parser.c` | INSERT/SELECT 변형, 문법 에러 15종, 에러 위치, 에러 후 다음 문장 계속 |
| 단위: B+ 트리 | `tests/test_bptree.c` | 순차/무작위 10만 건, order 3~5 분할 스트레스, **삽입할 때마다 불변식 검사**, 중복 키, 범위 순회 |
| 통합: 엔진 | `tests/test_engine.c` | 의미 에러 13종, 여러 행 원자성, 특수 문자열 왕복, **인덱스 결과 = 풀 스캔 결과**, LIMIT, 재시작 후 id 연속, 크래시 복구 2종, 손상 파일 거부, **쓰기 8 + 읽기 4 스레드 동시 실행** |
| 기능: API | `tests/api_test.py` | 실제 서버 프로세스: 정상 요청, 여러 문장, 유니코드, 400/404/405/411/413, **병렬 INSERT 300건 일관성**, 읽기·쓰기 혼합, graceful shutdown, 재시작 후 데이터 유지, **큐 포화 시 503** |

`make asan`으로 AddressSanitizer + UBSan 빌드를 만들어 같은 테스트를 돌릴 수 있습니다.

---

## 한계와 다음 단계

솔직하게 적어 둔 현재 한계입니다. 발표 질문에 대비할 때도 쓸 수 있습니다.

- **내구성**: `fflush`(OS 버퍼까지)만 하고 `fsync`는 하지 않습니다. 프로세스 크래시에는 안전하지만 OS나 전원 장애 시에는 최근 쓰기가 사라질 수 있습니다. 문장마다 `fsync`하면 대량 삽입이 크게 느려지는 트레이드오프가 있습니다.
- **체크섬 없음**: 줄 끝(`\n`)과 묶음 개수로 "덜 쓰인 것"은 잡지만, 길이는 맞는데 내용이 깨진 경우는 파싱이 실패할 때만 잡힙니다.
- **NULL, DELETE, UPDATE, AND/OR** 미지원. 현재 id → offset이 사실상 순서대로라 DELETE/UPDATE가 생기면 인덱스의 가치가 더 커집니다.
- **순차 삽입 시 리프가 절반만 참**: id가 항상 증가하므로 오른쪽 끝 분할 최적화(꽉 채워 분할)를 하면 노드 수를 절반으로 줄일 수 있습니다.
- **HTTP**: `Connection: close`만 지원합니다(keep-alive와 chunked 미지원).
- **큰 결과**: `SELECT *`로 100만 행을 API로 요청하면 결과 전체를 메모리에 만든 뒤 보냅니다. `LIMIT`을 쓰세요.
