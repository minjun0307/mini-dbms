/*
 * executor.h — [Phase 1~3] 실행기 + DB 엔진의 바깥 인터페이스
 *
 * Statement(파싱 결과)를 받아 실제로 실행한다.
 *   - 의미 검사: 테이블 존재, 컬럼 존재, 값 개수, 타입      (Parser 가 모르는 것들)
 *   - INSERT: 모든 행을 먼저 검증한 뒤 → 쓰기 락 → id 부여 + 파일에 추가 + 인덱스 갱신
 *   - SELECT: 읽기 락 → 실행 계획 선택
 *        WHERE id (=, <, <=, >, >=)  → B+ 트리 (INDEX LOOKUP / INDEX RANGE SCAN)
 *        그 외                        → 파일 처음부터 끝까지 (FULL SCAN = 선형 탐색)
 *
 * CLI(cli.c), 벤치마크(bench.c), API 서버(server.c) 모두 이 인터페이스 하나만 쓴다.
 * 즉 Phase 1 의 SQL 처리기가 Phase 2, 3 의 "내부 엔진"이 된다.
 */
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdio.h>

#include "parser.h"
#include "table.h"
#include "util.h"

typedef struct DB DB;

typedef struct {
    int ok;
    char error[ERR_MAX];
    StmtType type;
    char table[NAME_MAX_LEN];

    /* INSERT 결과 */
    long long inserted;
    long long first_id, last_id;

    /* SELECT 결과: nrows x ncols 표 */
    int ncols;
    char colnames[MAX_COLUMNS][NAME_MAX_LEN];
    ColType coltypes[MAX_COLUMNS];
    long long nrows, cap;
    Value *cells;

    /* 실행 정보 (성능 비교용) */
    int used_index;
    long long examined;      /* 실제로 읽어 본 행 수 */
    char plan[192];
    double elapsed_ms;
} Result;

/* [옵티마이저] id 범위 조건(<, <=, >, >=)의 예상 결과가 전체 행의 이 비율을 넘으면
 * B+ 트리 대신 풀 스캔을 쓴다. 근거: 100만 건·10만 건 측정에서 결과가 약 14% 를 넘으면
 * 풀 스캔이 더 빨랐다 (인덱스는 행마다 seek + 한 줄 읽기 ≈ 0.6 µs, 풀 스캔은 순차 읽기 ≈ 0.09 µs).
 * 측정 방법: ./build/bench 의 [5] 항목 (bench 는 측정을 위해 이 값을 1.0 으로 바꿔 인덱스를 강제한다).
 * 기본값 0.14. 쿼리 실행 중에는 읽기만 하므로, 바꾸려면 쿼리를 실행하기 전에 바꾼다. */
extern double index_range_max_ratio;

/* dir 안의 모든 *.schema 를 찾아 테이블을 연다 (복구 + 인덱스 재구축 포함) */
DB *db_open(const char *dir, char *err, size_t errlen);
void db_close(DB *db);
Table *db_find_table(DB *db, const char *name);
int db_table_count(const DB *db);
Table *db_table_at(DB *db, int i);

void result_init(Result *r);
void result_free(Result *r);

/* 문장 하나 실행 (스레드 안전) */
void db_execute(DB *db, const Statement *st, Result *r);

/* SQL 텍스트(여러 문장 가능)를 파싱+실행하고, 문장마다 cb 를 부른다. 반환값 = 실패한 문장 수 */
typedef void (*ResultCallback)(const Result *r, void *ctx);
int db_execute_script(DB *db, const char *sql, ResultCallback cb, void *ctx);

void result_print(const Result *r, FILE *out);     /* 사람이 읽는 표 형식 */
void result_to_json(const Result *r, SBuf *out);   /* API 응답용 JSON */

#endif
