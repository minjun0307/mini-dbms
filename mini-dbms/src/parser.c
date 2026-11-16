#include "parser.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void value_free(Value *v) {
    if (v->type == VAL_TEXT) {
        free(v->s);
        v->s = NULL;
    }
}

const char *op_str(CmpOp op) {
    static const char *names[] = {"=", "!=", "<", "<=", ">", ">="};
    return names[op];
}

/* ---------- 토큰 커서 ---------- */

static void next(Parser *p) { p->cur = lexer_next(&p->lx); }

void parser_init(Parser *p, const char *src) {
    lexer_init(&p->lx, src);
    p->err[0] = '\0';
    next(p);
}

static void describe(const Token *t, char *buf, size_t n) {
    switch (t->type) {
    case TOK_EOF: snprintf(buf, n, "end of input"); break;
    case TOK_STRING: snprintf(buf, n, "string '%.40s'", t->text); break;
    case TOK_KEYWORD: snprintf(buf, n, "keyword %.40s", t->text); break;
    default: snprintf(buf, n, "'%.40s'", t->text); break;
    }
}

/* 에러를 기록하고 -1 을 돌려준다. 현재 토큰이 Lexer 에러라면 그 메시지를 우선한다. */
static int fail(Parser *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(Parser *p, const char *fmt, ...) {
    char msg[ERR_MAX - 64];
    if (p->cur.type == TOK_ERROR) {
        snprintf(msg, sizeof msg, "%s", p->cur.text);
    } else {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
    }
    snprintf(p->err, sizeof p->err, "line %d, col %d: %s", p->cur.line, p->cur.col, msg);
    return -1;
}

static int is_kw(const Parser *p, const char *kw) {
    return p->cur.type == TOK_KEYWORD && strcmp(p->cur.text, kw) == 0;
}

/* accept: 있으면 소비하고 1, 없으면 그대로 두고 0 (선택적인 문법 요소) */
static int accept(Parser *p, TokenType t) {
    if (p->cur.type != t) return 0;
    next(p);
    return 1;
}

static int accept_kw(Parser *p, const char *kw) {
    if (!is_kw(p, kw)) return 0;
    next(p);
    return 1;
}

/* expect: 반드시 있어야 한다. 없으면 문법 에러 */
static int expect(Parser *p, TokenType t, const char *what) {
    if (accept(p, t)) return 0;
    char d[96];
    describe(&p->cur, d, sizeof d);
    return fail(p, "expected %s but found %s", what, d);
}

static int expect_kw(Parser *p, const char *kw) {
    if (accept_kw(p, kw)) return 0;
    char d[96];
    describe(&p->cur, d, sizeof d);
    return fail(p, "expected keyword %s but found %s", kw, d);
}

static int expect_ident(Parser *p, char *out, const char *what) {
    if (p->cur.type != TOK_IDENT) {
        char d[96];
        describe(&p->cur, d, sizeof d);
        return fail(p, "expected %s but found %s", what, d);
    }
    if (strlen(p->cur.text) >= NAME_MAX_LEN)
        return fail(p, "%s is too long (max %d characters)", what, NAME_MAX_LEN - 1);
    strcpy(out, p->cur.text);
    next(p);
    return 0;
}

/* ---------- 문법 규칙 = 함수 ---------- */

/* value := INT | STRING */
static int parse_value(Parser *p, Value *out) {
    if (p->cur.type == TOK_INT) {
        errno = 0;
        long long v = strtoll(p->cur.text, NULL, 10);
        if (errno == ERANGE) return fail(p, "integer out of range: %s", p->cur.text);
        out->type = VAL_INT;
        out->i = v;
        out->s = NULL;
        next(p);
        return 0;
    }
    if (p->cur.type == TOK_STRING) {
        out->type = VAL_TEXT;
        out->i = 0;
        out->s = xstrdup(p->cur.text);
        next(p);
        return 0;
    }
    char d[96];
    describe(&p->cur, d, sizeof d);
    return fail(p, "expected a value (integer or 'string') but found %s", d);
}

static void free_values(Value *vals, int n) {
    for (int i = 0; i < n; i++) value_free(&vals[i]);
}

/* insert := INSERT INTO ident [ '(' ident {',' ident} ')' ] VALUES row {',' row} */
static int parse_insert(Parser *p, Statement *s) {
    s->type = STMT_INSERT;
    if (expect_kw(p, "INSERT") || expect_kw(p, "INTO")) return -1;
    if (expect_ident(p, s->table, "table name")) return -1;

    if (accept(p, TOK_LPAREN)) {  /* 컬럼 목록은 선택 사항 → accept */
        do {
            if (s->ncols >= MAX_COLUMNS) return fail(p, "too many columns (max %d)", MAX_COLUMNS);
            if (expect_ident(p, s->cols[s->ncols], "column name")) return -1;
            s->ncols++;
        } while (accept(p, TOK_COMMA));
        if (expect(p, TOK_RPAREN, "')' or ','")) return -1;
    }

    if (expect_kw(p, "VALUES")) return -1;

    int cap = 0;
    do {  /* row {',' row} */
        if (expect(p, TOK_LPAREN, "'('")) return -1;
        Value row[MAX_COLUMNS];
        int n = 0;
        do {  /* value {',' value} */
            if (n >= MAX_COLUMNS) {
                free_values(row, n);
                return fail(p, "too many values in one row (max %d)", MAX_COLUMNS);
            }
            if (parse_value(p, &row[n])) {
                free_values(row, n);
                return -1;
            }
            n++;
        } while (accept(p, TOK_COMMA));
        if (expect(p, TOK_RPAREN, "')' or ','")) {
            free_values(row, n);
            return -1;
        }
        if (s->nrows == 0) {
            s->nvals = n;
        } else if (n != s->nvals) {
            free_values(row, n);
            return fail(p, "row %d has %d values but row 1 has %d", s->nrows + 1, n, s->nvals);
        }
        if (s->nrows == cap) {
            cap = cap ? cap * 2 : 4;
            s->values = xrealloc(s->values, sizeof(Value) * (size_t)cap * (size_t)s->nvals);
        }
        memcpy(&s->values[(size_t)s->nrows * (size_t)s->nvals], row, sizeof(Value) * (size_t)n);
        s->nrows++;
    } while (accept(p, TOK_COMMA));
    return 0;
}

/* select := SELECT ('*' | ident {',' ident}) FROM ident [WHERE ident op value] [LIMIT int] */
static int parse_select(Parser *p, Statement *s) {
    s->type = STMT_SELECT;
    if (expect_kw(p, "SELECT")) return -1;

    if (accept(p, TOK_STAR)) {
        s->select_all = 1;
    } else {
        do {
            if (s->nproj >= MAX_COLUMNS) return fail(p, "too many columns (max %d)", MAX_COLUMNS);
            if (expect_ident(p, s->proj[s->nproj], "column name or '*'")) return -1;
            s->nproj++;
        } while (accept(p, TOK_COMMA));
    }

    if (expect_kw(p, "FROM")) return -1;
    if (expect_ident(p, s->table, "table name")) return -1;

    if (accept_kw(p, "WHERE")) {
        s->has_where = 1;
        if (expect_ident(p, s->where_col, "column name")) return -1;
        switch (p->cur.type) {
        case TOK_EQ: s->where_op = OP_EQ; break;
        case TOK_NE: s->where_op = OP_NE; break;
        case TOK_LT: s->where_op = OP_LT; break;
        case TOK_LE: s->where_op = OP_LE; break;
        case TOK_GT: s->where_op = OP_GT; break;
        case TOK_GE: s->where_op = OP_GE; break;
        default: {
            char d[96];
            describe(&p->cur, d, sizeof d);
            return fail(p, "expected comparison operator (=, !=, <, <=, >, >=) but found %s", d);
        }
        }
        next(p);
        if (parse_value(p, &s->where_val)) return -1;
    }

    if (accept_kw(p, "LIMIT")) {
        if (p->cur.type != TOK_INT || p->cur.text[0] == '-')
            return fail(p, "LIMIT expects a non-negative integer");
        errno = 0;
        long long v = strtoll(p->cur.text, NULL, 10);
        if (errno == ERANGE) return fail(p, "LIMIT value out of range");
        s->limit = v;
        next(p);
    }
    return 0;
}

int parser_next(Parser *p, Statement *s) {
    memset(s, 0, sizeof *s);
    s->limit = -1;
    p->err[0] = '\0';

    while (accept(p, TOK_SEMICOLON)) {}  /* 빈 문장 ";;" 무시 */
    if (p->cur.type == TOK_EOF) return 0;

    s->line = p->cur.line;
    int rc;
    if (is_kw(p, "INSERT")) {
        rc = parse_insert(p, s);
    } else if (is_kw(p, "SELECT")) {
        rc = parse_select(p, s);
    } else {
        char d[96];
        describe(&p->cur, d, sizeof d);
        rc = fail(p, "expected INSERT or SELECT but found %s", d);
    }

    /* 문장 끝: ';' 또는 입력 끝 */
    if (rc == 0 && !accept(p, TOK_SEMICOLON) && p->cur.type != TOK_EOF) {
        char d[96];
        describe(&p->cur, d, sizeof d);
        rc = fail(p, "expected ';' but found %s", d);
    }

    if (rc != 0) {
        stmt_free(s);
        /* 에러 복구: 이 문장의 끝(';')까지 건너뛰어서 다음 문장은 계속 처리할 수 있게 한다 */
        while (p->cur.type != TOK_SEMICOLON && p->cur.type != TOK_EOF) next(p);
        accept(p, TOK_SEMICOLON);
        return -1;
    }
    return 1;
}

void stmt_free(Statement *s) {
    if (s->values) {
        free_values(s->values, s->nrows * s->nvals);
        free(s->values);
        s->values = NULL;
    }
    s->nrows = 0;
    value_free(&s->where_val);
}
