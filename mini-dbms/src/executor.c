#include "executor.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TABLES 64

/* 테이블 목록은 db_open 때 한 번 만들고 이후로 바뀌지 않는다 → 조회에 락이 필요 없다 */
struct DB {
    char dir[400];
    Table *tables[MAX_TABLES];
    int ntables;
};

/* ---------- DB 열기/닫기 ---------- */

DB *db_open(const char *dir, char *err, size_t errlen) {
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errlen, "cannot open data directory '%s': %s", dir, strerror(errno));
        return NULL;
    }
    DB *db = xcalloc(1, sizeof *db);
    snprintf(db->dir, sizeof db->dir, "%s", dir);

    const char *suffix = ".schema";
    size_t slen = strlen(suffix);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= slen || strcmp(e->d_name + len - slen, suffix) != 0) continue;
        char name[NAME_MAX_LEN];
        if (len - slen >= sizeof name) continue;
        memcpy(name, e->d_name, len - slen);
        name[len - slen] = '\0';
        if (db->ntables >= MAX_TABLES) {
            snprintf(err, errlen, "too many tables (max %d)", MAX_TABLES);
            rc = -1;
            break;
        }
        Table *t = table_open(dir, name, err, errlen);
        if (!t) {
            rc = -1;
            break;
        }
        if (t->recovered_bytes > 0)
            fprintf(stderr, "[recovery] table '%s': removed %lld bytes of incomplete data from %s\n",
                    t->name, t->recovered_bytes, t->data_path);
        db->tables[db->ntables++] = t;
    }
    closedir(d);
    if (rc != 0) {
        db_close(db);
        return NULL;
    }
    return db;
}

void db_close(DB *db) {
    if (!db) return;
    for (int i = 0; i < db->ntables; i++) table_close(db->tables[i]);
    free(db);
}

Table *db_find_table(DB *db, const char *name) {
    for (int i = 0; i < db->ntables; i++)
        if (str_ieq(db->tables[i]->name, name)) return db->tables[i];
    return NULL;
}

int db_table_count(const DB *db) { return db->ntables; }
Table *db_table_at(DB *db, int i) { return (i >= 0 && i < db->ntables) ? db->tables[i] : NULL; }

/* ---------- Result ---------- */

void result_init(Result *r) {
    memset(r, 0, sizeof *r);
    r->ok = 1;
}

void result_free(Result *r) {
    if (r->cells) {
        for (long long i = 0; i < r->nrows * r->ncols; i++) value_free(&r->cells[i]);
        free(r->cells);
        r->cells = NULL;
    }
    r->nrows = 0;
}

static void set_error(Result *r, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_error(Result *r, int line, const char *fmt, ...) {
    char msg[ERR_MAX - 32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    r->ok = 0;
    snprintf(r->error, sizeof r->error, "line %d: %s", line, msg);
}

/* 결과 표에 행 하나 추가 (필요한 컬럼만 골라서 = 투영, projection) */
static void result_add_row(Result *r, const Value *row, const int *proj) {
    if (r->nrows == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->cells = xrealloc(r->cells, sizeof(Value) * (size_t)r->cap * (size_t)r->ncols);
    }
    Value *dst = &r->cells[r->nrows * r->ncols];
    for (int j = 0; j < r->ncols; j++) {
        const Value *v = &row[proj[j]];
        dst[j].type = v->type;
        dst[j].i = v->i;
        dst[j].s = v->type == VAL_TEXT ? xstrdup(v->s) : NULL;
    }
    r->nrows++;
}

static int value_matches(const Value *v, CmpOp op, const Value *rhs) {
    int c = v->type == VAL_INT ? (v->i > rhs->i) - (v->i < rhs->i) : strcmp(v->s, rhs->s);
    switch (op) {
    case OP_EQ: return c == 0;
    case OP_NE: return c != 0;
    case OP_LT: return c < 0;
    case OP_LE: return c <= 0;
    case OP_GT: return c > 0;
    case OP_GE: return c >= 0;
    }
    return 0;
}

/* ---------- INSERT ---------- */

static void exec_insert(DB *db, const Statement *st, Result *r) {
    Table *t = db_find_table(db, st->table);
    if (!t) {
        set_error(r, st->line, "table '%s' does not exist", st->table);
        return;
    }
    const Schema *sc = &t->schema;
    int nc = sc->ncols;
    int expected = nc - 1;  /* id 는 자동 부여되므로 빼고 */
    int map[MAX_COLUMNS];   /* 값의 j번째 → 스키마의 몇 번째 컬럼인가 */

    if (st->ncols == 0) {
        if (st->nvals != expected) {
            char names[256] = "";
            for (int c = 1; c < nc; c++) {
                if (c > 1) strncat(names, ", ", sizeof names - strlen(names) - 1);
                strncat(names, sc->cols[c].name, sizeof names - strlen(names) - 1);
            }
            set_error(r, st->line, "table '%s' expects %d values (%s) but got %d; id is assigned automatically",
                      t->name, expected, names, st->nvals);
            return;
        }
        for (int j = 0; j < st->nvals; j++) map[j] = j + 1;
    } else {
        int seen[MAX_COLUMNS] = {0};
        for (int j = 0; j < st->ncols; j++) {
            int c = schema_find(sc, st->cols[j]);
            if (c < 0) {
                set_error(r, st->line, "unknown column '%s' in table '%s'", st->cols[j], t->name);
                return;
            }
            if (c == 0) {
                set_error(r, st->line, "column 'id' is assigned automatically and cannot be inserted");
                return;
            }
            if (seen[c]) {
                set_error(r, st->line, "column '%s' is listed more than once", sc->cols[c].name);
                return;
            }
            seen[c] = 1;
            map[j] = c;
        }
        for (int c = 1; c < nc; c++) {
            if (!seen[c]) {
                set_error(r, st->line, "missing value for column '%s' (NULL is not supported)", sc->cols[c].name);
                return;
            }
        }
        if (st->nvals != st->ncols) {
            set_error(r, st->line, "%d columns listed but %d values given", st->ncols, st->nvals);
            return;
        }
    }

    /* 쓰기 전에 모든 행을 검증한다 → 하나라도 틀리면 아무것도 쓰지 않음 (문장 단위 원자성) */
    for (int i = 0; i < st->nrows; i++) {
        for (int j = 0; j < st->nvals; j++) {
            const Value *v = &st->values[(size_t)i * st->nvals + j];
            const Column *col = &sc->cols[map[j]];
            if (col->type == COL_INT && v->type != VAL_INT) {
                set_error(r, st->line, "row %d: column '%s' is INT but got string '%.40s'", i + 1, col->name, v->s);
                return;
            }
            if (col->type == COL_TEXT && v->type != VAL_TEXT) {
                set_error(r, st->line, "row %d: column '%s' is TEXT but got integer %lld", i + 1, col->name, v->i);
                return;
            }
            if (v->type == VAL_TEXT && strpbrk(v->s, "\r\n")) {
                set_error(r, st->line, "row %d: column '%s': line breaks are not allowed in TEXT", i + 1, col->name);
                return;
            }
        }
    }

    /* Statement 의 값 순서 → 스키마 순서로 재배치 (문자열은 복사하지 않고 빌려 쓴다) */
    Value *rows = xcalloc((size_t)st->nrows * nc, sizeof(Value));
    for (int i = 0; i < st->nrows; i++)
        for (int j = 0; j < st->nvals; j++)
            rows[(size_t)i * nc + map[j]] = st->values[(size_t)i * st->nvals + j];

    char err[ERR_MAX];
    long long first_id = 0;
    pthread_rwlock_wrlock(&t->lock);   /* 쓰기 락: 이 테이블의 다른 읽기/쓰기를 모두 막는다 */
    int rc = table_append(t, rows, st->nrows, &first_id, err, sizeof err);
    pthread_rwlock_unlock(&t->lock);
    free(rows);

    if (rc != 0) {
        set_error(r, st->line, "%s", err);
        return;
    }
    r->inserted = st->nrows;
    r->first_id = first_id;
    r->last_id = first_id + st->nrows - 1;
    snprintf(r->plan, sizeof r->plan, "APPEND%s", st->nrows > 1 ? " (atomic batch)" : "");
}

/* ---------- SELECT ---------- */

double index_range_max_ratio = 0.14;

/* id 범위 조건의 결과 행 수를 데이터를 읽지 않고 계산한다.
 * id 는 1 씩 늘어나며 빈틈이 없다(append-only, 삭제 없음) → [가장 작은 id, 가장 작은 id + 행 수 - 1] */
static long long estimate_id_range_rows(const Table *t, CmpOp op, long long k) {
    BPIter it;
    bpt_first(t->index, &it);
    if (!bpt_iter_valid(&it)) return 0;
    long long lo = bpt_iter_key(&it), hi = lo + (long long)t->index->size - 1;
    long long from = lo, to = hi;
    switch (op) {
    case OP_LT: if (k <= lo) return 0; if (k - 1 < hi) to = k - 1; break;
    case OP_LE: if (k < lo) return 0; if (k < hi) to = k; break;
    case OP_GT: if (k >= hi) return 0; if (k + 1 > lo) from = k + 1; break;
    case OP_GE: if (k > hi) return 0; if (k > lo) from = k; break;
    default: return 1;
    }
    return to - from + 1;
}

static void exec_select(DB *db, const Statement *st, Result *r) {
    Table *t = db_find_table(db, st->table);
    if (!t) {
        set_error(r, st->line, "table '%s' does not exist", st->table);
        return;
    }
    const Schema *sc = &t->schema;

    int proj[MAX_COLUMNS];
    if (st->select_all) {
        r->ncols = sc->ncols;
        for (int j = 0; j < sc->ncols; j++) proj[j] = j;
    } else {
        r->ncols = st->nproj;
        for (int j = 0; j < st->nproj; j++) {
            proj[j] = schema_find(sc, st->proj[j]);
            if (proj[j] < 0) {
                set_error(r, st->line, "unknown column '%s' in table '%s'", st->proj[j], t->name);
                return;
            }
        }
    }
    for (int j = 0; j < r->ncols; j++) {
        strcpy(r->colnames[j], sc->cols[proj[j]].name);
        r->coltypes[j] = sc->cols[proj[j]].type;
    }

    int wc = -1;
    if (st->has_where) {
        wc = schema_find(sc, st->where_col);
        if (wc < 0) {
            set_error(r, st->line, "unknown column '%s' in WHERE clause", st->where_col);
            return;
        }
        if (sc->cols[wc].type == COL_INT && st->where_val.type != VAL_INT) {
            set_error(r, st->line, "column '%s' is INT but was compared with string '%.40s'",
                      sc->cols[wc].name, st->where_val.s);
            return;
        }
        if (sc->cols[wc].type == COL_TEXT && st->where_val.type != VAL_TEXT) {
            set_error(r, st->line, "column '%s' is TEXT but was compared with integer %lld",
                      sc->cols[wc].name, st->where_val.i);
            return;
        }
    }

    long long limit = st->limit;
    int use_index = st->has_where && wc == 0 && st->where_op != OP_NE;
    char err[ERR_MAX] = "";
    Value row[MAX_COLUMNS];
    TableReader rd;

    pthread_rwlock_rdlock(&t->lock);   /* 읽기 락: 다른 SELECT 와는 동시에, INSERT 와는 배타적으로 */
    if (reader_open(t, &rd, err, sizeof err) != 0) {
        pthread_rwlock_unlock(&t->lock);
        set_error(r, st->line, "%s", err);
        return;
    }

    /* [옵티마이저] 범위 조건은 결과가 많으면 풀 스캔이 더 빠르다 → 예상 결과 비율로 고른다.
     * LIMIT 이 있으면 인덱스는 그만큼만 읽고 멈추므로 예상 결과를 LIMIT 으로 줄여서 본다. */
    char why[96] = "";
    if (use_index && st->where_op != OP_EQ && t->index->size > 0) {
        long long est = estimate_id_range_rows(t, st->where_op, st->where_val.i);
        if (limit >= 0 && limit < est) est = limit;
        double ratio = (double)est / (double)t->index->size;
        if (ratio > index_range_max_ratio) use_index = 0;
        snprintf(why, sizeof why, " [optimizer: est. %.1f%% of rows %s %.0f%%]",
                 ratio * 100.0, use_index ? "<=" : ">", index_range_max_ratio * 100.0);
    }

    if (use_index) {
        /* [Phase 2] WHERE id ... → B+ 트리로 위치를 찾고 fseek 로 그 행만 읽는다 */
        long long k = st->where_val.i;
        r->used_index = 1;
        snprintf(r->plan, sizeof r->plan, "%s on id (B+ tree, id %s %lld)",
                 st->where_op == OP_EQ ? "INDEX LOOKUP" : "INDEX RANGE SCAN", op_str(st->where_op), k);
        if (st->where_op == OP_EQ) {
            bpval_t off;
            if (limit != 0 && bpt_search(t->index, k, &off)) {
                if (reader_read_at(t, &rd, off, row, err, sizeof err) == 0) {
                    r->examined++;
                    result_add_row(r, row, proj);
                    row_free(sc, row);
                }
            }
        } else {
            BPIter it;
            int empty = 0;
            if (st->where_op == OP_GT || st->where_op == OP_GE) {
                if (st->where_op == OP_GT && k == LLONG_MAX) empty = 1;
                else bpt_seek(t->index, st->where_op == OP_GT ? k + 1 : k, &it);
            } else {
                bpt_first(t->index, &it);
            }
            /* 리프 연결 리스트를 따라 정렬된 순서로 옆으로 훑는다 */
            for (; !empty && bpt_iter_valid(&it); bpt_iter_next(&it)) {
                bpkey_t key = bpt_iter_key(&it);
                if (st->where_op == OP_LT && key >= k) break;
                if (st->where_op == OP_LE && key > k) break;
                if (limit >= 0 && r->nrows >= limit) break;
                if (reader_read_at(t, &rd, bpt_iter_val(&it), row, err, sizeof err) != 0) break;
                r->examined++;
                result_add_row(r, row, proj);
                row_free(sc, row);
            }
        }
    } else {
        /* 선형 탐색: 파일을 처음부터 한 줄씩 읽으며  읽기 → 필터(WHERE) → 투영 */
        if (st->has_where) {
            const Value *v = &st->where_val;
            if (v->type == VAL_INT)
                snprintf(r->plan, sizeof r->plan, "FULL SCAN + filter (%s %s %lld)",
                         sc->cols[wc].name, op_str(st->where_op), v->i);
            else
                snprintf(r->plan, sizeof r->plan, "FULL SCAN + filter (%s %s '%.40s')",
                         sc->cols[wc].name, op_str(st->where_op), v->s);
        } else {
            snprintf(r->plan, sizeof r->plan, "FULL SCAN");
        }
        for (;;) {
            if (limit >= 0 && r->nrows >= limit) break;
            int rc = reader_next(t, &rd, row, NULL, err, sizeof err);
            if (rc <= 0) break;
            r->examined++;
            if (!st->has_where || value_matches(&row[wc], st->where_op, &st->where_val))
                result_add_row(r, row, proj);
            row_free(sc, row);
        }
    }

    if (why[0]) strncat(r->plan, why, sizeof r->plan - strlen(r->plan) - 1);

    reader_close(&rd);
    pthread_rwlock_unlock(&t->lock);
    if (err[0]) {
        result_free(r);
        set_error(r, st->line, "%s", err);
    }
}

void db_execute(DB *db, const Statement *st, Result *r) {
    double t0 = now_ms();
    r->type = st->type;
    snprintf(r->table, sizeof r->table, "%s", st->table);
    if (st->type == STMT_INSERT) exec_insert(db, st, r);
    else exec_select(db, st, r);
    r->elapsed_ms = now_ms() - t0;
}

int db_execute_script(DB *db, const char *sql, ResultCallback cb, void *ctx) {
    Parser p;
    Statement st;
    int errors = 0;
    parser_init(&p, sql);
    for (;;) {
        int rc = parser_next(&p, &st);  /* 입력 → 파싱 */
        if (rc == 0) break;
        Result res;
        result_init(&res);
        if (rc < 0) {
            res.ok = 0;
            snprintf(res.error, sizeof res.error, "%s", p.err);
        } else {
            db_execute(db, &st, &res);      /* → 실행 → 저장 */
            stmt_free(&st);
        }
        if (!res.ok) errors++;
        if (cb) cb(&res, ctx);
        result_free(&res);
    }
    return errors;
}

/* ---------- 출력: 표 ---------- */

/* 터미널에 보이는 폭 (한글 같은 전각 문자는 2칸) */
static int display_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p >> 5) == 0x6) { cp = *p & 0x1F; len = 2; }
        else if ((*p >> 4) == 0xE) { cp = *p & 0x0F; len = 3; }
        else if ((*p >> 3) == 0x1E) { cp = *p & 0x07; len = 4; }
        else { w++; p++; continue; }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += len;
        int wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                   (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                   (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
    }
    return w;
}

static const char *cell_text(const Value *v, char *buf, size_t n) {
    if (v->type == VAL_TEXT) return v->s;
    snprintf(buf, n, "%lld", v->i);
    return buf;
}

static void print_border(FILE *out, const int *w, int ncols) {
    for (int j = 0; j < ncols; j++) {
        fputc('+', out);
        for (int k = 0; k < w[j] + 2; k++) fputc('-', out);
    }
    fputs("+\n", out);
}

static void print_cell(FILE *out, const char *s, int width, int right) {
    int pad = width - display_width(s);
    fputs("| ", out);
    if (right) for (int k = 0; k < pad; k++) fputc(' ', out);
    fputs(s, out);
    if (!right) for (int k = 0; k < pad; k++) fputc(' ', out);
    fputc(' ', out);
}

void result_print(const Result *r, FILE *out) {
    if (!r->ok) {
        fprintf(out, "ERROR %s\n", r->error);
        return;
    }
    if (r->type == STMT_INSERT) {
        if (r->inserted == 1)
            fprintf(out, "Query OK, 1 row inserted into '%s' (id %lld) [%.3f ms]\n",
                    r->table, r->first_id, r->elapsed_ms);
        else
            fprintf(out, "Query OK, %lld rows inserted into '%s' (id %lld..%lld) [%.3f ms]\n",
                    r->inserted, r->table, r->first_id, r->last_id, r->elapsed_ms);
        return;
    }

    if (r->nrows > 0) {
        int w[MAX_COLUMNS];
        char buf[32];
        for (int j = 0; j < r->ncols; j++) w[j] = display_width(r->colnames[j]);
        for (long long i = 0; i < r->nrows; i++)
            for (int j = 0; j < r->ncols; j++) {
                int dw = display_width(cell_text(&r->cells[i * r->ncols + j], buf, sizeof buf));
                if (dw > w[j]) w[j] = dw;
            }
        print_border(out, w, r->ncols);
        for (int j = 0; j < r->ncols; j++) print_cell(out, r->colnames[j], w[j], 0);
        fputs("|\n", out);
        print_border(out, w, r->ncols);
        for (long long i = 0; i < r->nrows; i++) {
            for (int j = 0; j < r->ncols; j++) {
                const Value *v = &r->cells[i * r->ncols + j];
                print_cell(out, cell_text(v, buf, sizeof buf), w[j], v->type == VAL_INT);
            }
            fputs("|\n", out);
        }
        print_border(out, w, r->ncols);
    }
    fprintf(out, "%lld row%s in set [%.3f ms] plan: %s, examined %lld row%s\n",
            r->nrows, r->nrows == 1 ? "" : "s", r->elapsed_ms, r->plan,
            r->examined, r->examined == 1 ? "" : "s");
}

/* ---------- 출력: JSON ---------- */

static void json_string(SBuf *b, const char *s) {
    sb_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
            else sb_putc(b, (char)*p);
        }
    }
    sb_putc(b, '"');
}

void result_to_json(const Result *r, SBuf *b) {
    if (!r->ok) {
        sb_puts(b, "{\"ok\":false,\"error\":");
        json_string(b, r->error);
        sb_putc(b, '}');
        return;
    }
    if (r->type == STMT_INSERT) {
        sb_puts(b, "{\"ok\":true,\"type\":\"insert\",\"table\":");
        json_string(b, r->table);
        sb_printf(b, ",\"inserted\":%lld,\"first_id\":%lld,\"last_id\":%lld,\"elapsed_ms\":%.3f}",
                  r->inserted, r->first_id, r->last_id, r->elapsed_ms);
        return;
    }
    sb_puts(b, "{\"ok\":true,\"type\":\"select\",\"table\":");
    json_string(b, r->table);
    sb_puts(b, ",\"columns\":[");
    for (int j = 0; j < r->ncols; j++) {
        if (j) sb_putc(b, ',');
        json_string(b, r->colnames[j]);
    }
    sb_puts(b, "],\"rows\":[");
    for (long long i = 0; i < r->nrows; i++) {
        if (i) sb_putc(b, ',');
        sb_putc(b, '[');
        for (int j = 0; j < r->ncols; j++) {
            const Value *v = &r->cells[i * r->ncols + j];
            if (j) sb_putc(b, ',');
            if (v->type == VAL_INT) sb_printf(b, "%lld", v->i);
            else json_string(b, v->s);
        }
        sb_putc(b, ']');
    }
    sb_printf(b, "],\"row_count\":%lld,\"plan\":", r->nrows);
    json_string(b, r->plan);
    sb_printf(b, ",\"index_used\":%s,\"rows_examined\":%lld,\"elapsed_ms\":%.3f}",
              r->used_index ? "true" : "false", r->examined, r->elapsed_ms);
}
