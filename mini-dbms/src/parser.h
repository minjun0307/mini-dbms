/*
 * parser.h — [Phase 1] 재귀 하강 파서 (Recursive Descent Parser)
 *
 * 토큰을 앞에서부터 하나씩 읽으며 문법을 검사하고, 그 결과를 Statement 구조체로 만든다.
 *
 *   statement := insert | select
 *   insert    := INSERT INTO ident [ '(' ident {',' ident} ')' ]
 *                VALUES row {',' row} [';']
 *   row       := '(' value {',' value} ')'
 *   select    := SELECT ( '*' | ident {',' ident} ) FROM ident
 *                [ WHERE ident op value ] [ LIMIT int ] [';']
 *   op        := = | != | <> | < | <= | > | >=
 *   value     := INT | STRING
 *
 * 문법 규칙 하나당 함수 하나 (parse_insert, parse_select, parse_value ...)를 두고,
 * 큰 규칙에서 작은 규칙으로 함수를 타고 "내려간다(하강)".
 *
 * Parser 는 "문법"만 안다. 테이블에 컬럼이 몇 개인지, 타입이 맞는지는 모른다.
 * 그런 "의미" 검사는 Executor 의 몫이다.
 */
#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "util.h"

typedef enum { VAL_INT, VAL_TEXT } ValueType;

typedef struct {
    ValueType type;
    long long i;   /* VAL_INT */
    char *s;       /* VAL_TEXT (malloc 된 문자열) */
} Value;

void value_free(Value *v);

typedef enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE } CmpOp;
const char *op_str(CmpOp op);

typedef enum { STMT_INSERT, STMT_SELECT } StmtType;

/* 파싱 결과 (AST). WHERE 조건이 하나뿐이라 트리 대신 평평한 구조체로 충분하다. */
typedef struct {
    StmtType type;
    int line;                               /* 문장이 시작된 줄 (에러 메시지용) */
    char table[NAME_MAX_LEN];

    /* INSERT */
    int ncols;                              /* 0 이면 컬럼 목록 생략 */
    char cols[MAX_COLUMNS][NAME_MAX_LEN];
    int nrows;                              /* VALUES (...), (...) 의 행 개수 */
    int nvals;                              /* 행 하나의 값 개수 */
    Value *values;                          /* nrows * nvals 개, 행 우선 배치 */

    /* SELECT */
    int select_all;                         /* SELECT * */
    int nproj;
    char proj[MAX_COLUMNS][NAME_MAX_LEN];
    int has_where;
    char where_col[NAME_MAX_LEN];
    CmpOp where_op;
    Value where_val;
    long long limit;                        /* -1 = LIMIT 없음 */
} Statement;

/* Parser 는 Lexer 에게 토큰을 "필요할 때마다" 하나씩 받아 온다 (스트리밍).
 * 100만 줄짜리 SQL 파일도 토큰 전체를 메모리에 올리지 않고 처리할 수 있다. */
typedef struct {
    Lexer lx;
    Token cur;            /* 지금 보고 있는 토큰 (1개 미리보기) */
    char err[ERR_MAX];
} Parser;

void parser_init(Parser *p, const char *src);

/* 다음 문장 하나를 파싱한다.
 *   1  : out 에 문장을 채움 (사용 후 stmt_free 필요)
 *   0  : 더 이상 문장이 없음
 *  -1  : 문법 에러 (p->err 에 메시지). 다음 ';' 까지 건너뛰었으므로 계속 호출해도 된다. */
int parser_next(Parser *p, Statement *out);
void stmt_free(Statement *s);

#endif
