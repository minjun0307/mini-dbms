/*
 * table.h — [Phase 1] 파일 기반 저장소 (Storage)
 *
 * 테이블 하나 = 파일 두 개
 *   data/users.schema   컬럼 정의 (사람이 직접 작성. CREATE TABLE 은 구현하지 않음)
 *       id INT
 *       name TEXT
 *       age INT
 *   data/users.tbl      실제 데이터 (CSV 형태, 한 줄 = 한 행)
 *       1,"kim",20
 *       2,"O""Brien, Jr",31          ← 문자열은 항상 "" 로 감싸고, 안의 " 는 "" 로 이스케이프
 *       @B 2                         ← 여러 행 INSERT 묶음 시작 표시 (뒤따르는 2줄이 한 묶음)
 *       3,"lee",25
 *       4,"park",40
 *
 * 설계 포인트
 *   - 가변 길이 행이라 "n번째 행"으로 바로 갈 수 없다
 *     → [Phase 2] B+ 트리에 id → 바이트 offset 을 저장해서 fseek 한 번으로 찾아간다.
 *   - INSERT 는 항상 파일 끝에 추가(append)만 한다. 기존 데이터를 덮어쓰지 않는다.
 *   - 크래시 복구: 열 때 파일을 끝까지 읽으며
 *       (1) '\n' 으로 끝나지 않은 마지막 줄 (쓰다가 중단)
 *       (2) "@B n" 뒤에 n 줄이 다 오지 않은 묶음
 *     을 발견하면 그 시작 위치에서 파일을 잘라낸다 → 여러 행 INSERT 는 "전부 아니면 전무".
 *   - B+ 트리는 메모리에만 있으므로, 열 때 파일을 훑으며 다시 만든다.
 *
 * 동시성: Table 마다 읽기/쓰기 락(rwlock)이 있다. 이 파일의 함수들은 락을 잡지 않으며,
 *         호출자(executor)가 SELECT 는 읽기 락, INSERT 는 쓰기 락을 잡고 부른다.
 */
#ifndef TABLE_H
#define TABLE_H

#include <pthread.h>
#include <stdio.h>

#include "bptree.h"
#include "parser.h"
#include "util.h"

typedef enum { COL_INT, COL_TEXT } ColType;

typedef struct {
    char name[NAME_MAX_LEN];
    ColType type;
} Column;

typedef struct {
    int ncols;
    Column cols[MAX_COLUMNS];   /* cols[0] 은 항상 "id INT" (자동 부여) */
} Schema;

typedef struct {
    char name[NAME_MAX_LEN];
    char schema_path[512];
    char data_path[512];
    Schema schema;
    FILE *wfp;                  /* append 전용 핸들 */
    BPTree *index;              /* id → 바이트 offset */
    long long next_id;          /* 다음에 부여할 id */
    long long nrows;
    long long file_size;        /* 커밋된(완전히 쓰인) 데이터의 끝 위치 */
    long long recovered_bytes;  /* 열 때 잘라낸 불완전 데이터 크기 (0 이면 정상) */
    pthread_rwlock_t lock;
} Table;

const char *coltype_name(ColType t);
int schema_load(const char *path, Schema *out, char *err, size_t errlen);
int schema_find(const Schema *s, const char *name);   /* 컬럼 번호 또는 -1 (대소문자 무시) */

Table *table_open(const char *dir, const char *name, char *err, size_t errlen);
void table_close(Table *t);

/* 행 직렬화 / 역직렬화 */
void row_encode(const Schema *s, const Value *row, SBuf *out);                 /* '\n' 까지 붙인다 */
int row_decode(const Schema *s, const char *line, size_t len, Value *row);    /* line 은 NUL 종료 */
void row_free(const Schema *s, Value *row);

/* rows: nrows * ncols 개 (컬럼 0 = id 자리는 여기서 채운다). 쓰기 락 필요.
 * 모든 행을 버퍼에 직렬화한 뒤 한 번에 기록하고, 실패하면 잘라내서 되돌린다. */
int table_append(Table *t, Value *rows, int nrows, long long *first_id, char *err, size_t errlen);

/* 읽기 커서. 읽기 락을 잡은 상태에서 사용한다. */
typedef struct {
    FILE *fp;
    char *buf;
    size_t cap;
    long long pos;
    long long end;   /* 열 때의 file_size 스냅샷 */
} TableReader;

int reader_open(Table *t, TableReader *r, char *err, size_t errlen);
void reader_close(TableReader *r);
/* 순차 읽기 (풀 스캔용). 1 = row 채움, 0 = 끝, -1 = 에러 */
int reader_next(Table *t, TableReader *r, Value *row, long long *offset, char *err, size_t errlen);
/* offset 위치의 행 하나를 바로 읽기 (인덱스용). 0 = 성공 */
int reader_read_at(Table *t, TableReader *r, long long offset, Value *row, char *err, size_t errlen);

#endif
