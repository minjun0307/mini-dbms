/*
 * bench.c — [Phase 2] 대용량 삽입 + 인덱스 사용/미사용 성능 비교
 *
 *   ./build/bench [-n ROWS] [-q QUERIES] [-b BATCH] [-d DIR]
 *
 * 1) 빈 테이블을 만들고 N 개의 행을 "SQL 문으로" 삽입한다 (Phase 1 처리기를 그대로 사용)
 * 2) DB 를 닫았다가 다시 열어서 인덱스 재구축 시간을 잰다
 * 3) 같은 행을 두 가지 방법으로 찾는다
 *      SELECT * FROM users WHERE id = k            → B+ 트리
 *      SELECT * FROM users WHERE name = 'user_k'   → 선형 탐색 (풀 스캔)
 *    결과가 같은지 확인하고 시간을 비교한다.
 * 4) 범위 검색과 인덱스 없는 컬럼 검색
 * 5) 결과 비율별로 인덱스 범위 검색 vs 풀 스캔을 재서 옵티마이저 기준값의 근거를 보인다
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "executor.h"

typedef struct {
    int errors;
    char err[ERR_MAX];
    long long nrows;
    long long examined;
    int used_index;
    long long first_id;
    double ms;
    char plan[192];
} Capture;

static void capture(const Result *r, void *ctx) {
    Capture *c = ctx;
    if (!r->ok) {
        c->errors++;
        snprintf(c->err, sizeof c->err, "%s", r->error);
        return;
    }
    c->ms += r->elapsed_ms;
    c->nrows = r->type == STMT_SELECT ? r->nrows : r->inserted;
    c->examined = r->examined;
    c->used_index = r->used_index;
    c->first_id = (r->type == STMT_SELECT && r->nrows > 0) ? r->cells[0].i : -1;
    snprintf(c->plan, sizeof c->plan, "%s", r->plan);
}

static unsigned long long rng_state = 0x9E3779B97F4A7C15ULL;
static unsigned long long rng(void) {  /* xorshift64: 재현 가능한 난수 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static const char *commas(long long v, char *buf) {  /* 1234567 → "1,234,567" */
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", v), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
    return buf;
}

static void die(const char *msg) {
    fprintf(stderr, "bench: %s\n", msg);
    exit(1);
}

static double best_of(DB *db, const char *q, int reps, Capture *out) {
    double best = 1e18;
    for (int i = 0; i < reps; i++) {
        Capture c = {0};
        db_execute_script(db, q, capture, &c);
        if (c.errors) die(c.err);
        if (c.ms < best) best = c.ms;
        *out = c;
    }
    return best;
}

/* 5) 옵티마이저 기준값(index_range_max_ratio)의 근거: 결과 비율별 인덱스 범위 검색 vs 풀 스캔.
 * 같은 행 집합을 두 방법으로 고른다.
 *   WHERE id > X  → B+ 트리 범위 검색 (측정 동안 기준값을 1.0 으로 올려 인덱스를 강제)
 *   WHERE k  > X  → 풀 스캔 (k 는 id 와 같은 값을 넣은, 인덱스 없는 컬럼) */
static void bench_range_ratio(const char *dir, long long n, int batch) {
    char path[512], err[ERR_MAX], a[32];
    snprintf(path, sizeof path, "%s/ranges.schema", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write schema");
    fputs("id INT\nk INT\nname TEXT\nage INT\nemail TEXT\n", fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/ranges.tbl", dir);
    unlink(path);
    DB *db = db_open(dir, err, sizeof err);
    if (!db) die(err);

    SBuf sql;
    sb_init(&sql);
    Capture ins = {0};
    for (long long start = 1; start <= n; start += batch) {
        long long end = start + batch - 1 < n ? start + batch - 1 : n;
        sb_reset(&sql);
        sb_puts(&sql, "INSERT INTO ranges (k, name, age, email) VALUES ");
        for (long long i = start; i <= end; i++)
            sb_printf(&sql, "%s(%lld, 'user_%lld', %lld, 'user_%lld@example.com')",
                      i == start ? "" : ", ", i, i, 18 + (i * 7) % 60, i);
        sb_puts(&sql, ";");
        db_execute_script(db, sql.data, capture, &ins);
        if (ins.errors) die(ins.err);
    }
    sb_free(&sql);

    double saved = index_range_max_ratio;
    double pct[] = {1, 5, 10, 12, 14, 16, 20, 50, 100};
    double crossover = -1;
    printf("\n[5] Range query: index vs full scan by result size (best of 3, SELECT *)\n");
    printf("    %8s %12s %12s %12s %8s   %s\n", "result", "rows", "index ms", "scan ms", "faster",
           "optimizer picks");
    for (size_t i = 0; i < sizeof pct / sizeof pct[0]; i++) {
        long long x = n - (long long)(n * pct[i] / 100.0);
        char q[160];
        Capture ci, cs, co = {0};
        index_range_max_ratio = 1.0;  /* 인덱스 강제 */
        snprintf(q, sizeof q, "SELECT * FROM ranges WHERE id > %lld;", x);
        double ti = best_of(db, q, 3, &ci);
        index_range_max_ratio = saved;
        db_execute_script(db, q, capture, &co);  /* 기본 기준값이면 무엇을 고르는지 */
        snprintf(q, sizeof q, "SELECT * FROM ranges WHERE k > %lld;", x);
        double ts = best_of(db, q, 3, &cs);
        if (ci.nrows != cs.nrows || !ci.used_index || cs.used_index) die("range benchmark: plan or result mismatch");
        if (crossover < 0 && ts < ti) crossover = pct[i];
        printf("    %7.0f%% %12s %12.2f %12.2f %8s   %s\n", pct[i], commas(ci.nrows, a), ti, ts,
               ti <= ts ? "index" : "scan", co.used_index ? "index" : "full scan");
    }
    index_range_max_ratio = saved;
    if (crossover > 0)
        printf("    => full scan wins from about %.0f%% of rows (optimizer threshold: %.0f%%)\n",
               crossover, saved * 100.0);
    else
        printf("    => index won at every measured size (optimizer threshold: %.0f%%)\n", saved * 100.0);
    db_close(db);
    /* 다음 실행의 [1]·[2] 측정에 섞이지 않도록 측정용 테이블은 지운다 */
    unlink(path);
    snprintf(path, sizeof path, "%s/ranges.schema", dir);
    unlink(path);
}

int main(int argc, char **argv) {
    long long n = 1000000;
    int queries = 10, batch = 1000;
    const char *dir = "bench_data";
    int opt;
    while ((opt = getopt(argc, argv, "n:q:b:d:h")) != -1) {
        switch (opt) {
        case 'n': n = atoll(optarg); break;
        case 'q': queries = atoi(optarg); break;
        case 'b': batch = atoi(optarg); break;
        case 'd': dir = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-n ROWS] [-q QUERIES] [-b BATCH] [-d DIR]\n", argv[0]);
            return 2;
        }
    }
    if (n < 1 || queries < 1 || batch < 1) die("invalid arguments");

    /* 새 테이블 준비 */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) die("cannot create data directory");
    char path[512];
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) die("cannot write schema");
    fputs("id INT\nname TEXT\nage INT\nemail TEXT\n", fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/users.tbl", dir);
    unlink(path);

    char err[ERR_MAX], a[32], b[32];
    DB *db = db_open(dir, err, sizeof err);
    if (!db) die(err);

    printf("=== mini-dbms benchmark: %s rows ===\n\n", commas(n, a));

    /* 1) 대량 삽입: SQL 텍스트를 만들어 Phase 1 처리기로 실행 */
    SBuf sql;
    sb_init(&sql);
    Capture ins = {0};
    double t0 = now_ms();
    for (long long start = 1; start <= n; start += batch) {
        long long end = start + batch - 1 < n ? start + batch - 1 : n;
        sb_reset(&sql);
        sb_puts(&sql, "INSERT INTO users (name, age, email) VALUES ");
        for (long long i = start; i <= end; i++)
            sb_printf(&sql, "%s('user_%lld', %lld, 'user_%lld@example.com')",
                      i == start ? "" : ", ", i, 18 + (i * 7) % 60, i);
        sb_puts(&sql, ";");
        db_execute_script(db, sql.data, capture, &ins);
        if (ins.errors) die(ins.err);
        if ((start / batch) % 100 == 0) {
            fprintf(stderr, "\r  inserting... %lld / %lld", end, n);
            fflush(stderr);
        }
    }
    double insert_ms = now_ms() - t0;
    fprintf(stderr, "\r%60s\r", "");
    sb_free(&sql);

    struct stat stbuf;
    stat(path, &stbuf);
    printf("[1] INSERT %s rows via SQL (%d rows per statement)\n", commas(n, a), batch);
    printf("    total %.1f ms, %s rows/sec, data file %.1f MB\n\n",
           insert_ms, commas((long long)(n / (insert_ms / 1000.0)), b), stbuf.st_size / 1048576.0);

    /* 2) 다시 열기: 파일을 훑어서 B+ 트리 재구축 */
    db_close(db);
    t0 = now_ms();
    db = db_open(dir, err, sizeof err);
    double reopen_ms = now_ms() - t0;
    if (!db) die(err);
    Table *t = db_find_table(db, "users");
    char cerr[256];
    if (!bpt_check(t->index, cerr, sizeof cerr)) die(cerr);
    printf("[2] Reopen: scan file + rebuild B+ tree index\n");
    printf("    %.1f ms, %s keys, height %d, %s nodes (order %d), invariants OK\n\n",
           reopen_ms, commas((long long)t->index->size, a), t->index->height,
           commas((long long)t->index->nodes, b), t->index->order);

    /* 3) 같은 행을 인덱스 vs 선형 탐색으로 찾기 */
    double idx_sum = 0, idx_min = 1e18, idx_max = 0;
    double scan_sum = 0, scan_min = 1e18, scan_max = 0;
    long long idx_examined = 0, scan_examined = 0;
    char q[256];
    for (int i = 0; i < queries; i++) {
        long long k = 1 + (long long)(rng() % (unsigned long long)n);

        Capture ci = {0};
        snprintf(q, sizeof q, "SELECT * FROM users WHERE id = %lld;", k);
        db_execute_script(db, q, capture, &ci);

        Capture cs = {0};
        snprintf(q, sizeof q, "SELECT * FROM users WHERE name = 'user_%lld';", k);
        db_execute_script(db, q, capture, &cs);

        if (ci.errors || cs.errors) die(ci.errors ? ci.err : cs.err);
        if (ci.nrows != 1 || cs.nrows != 1 || ci.first_id != k || cs.first_id != k)
            die("index and full scan returned different results");
        if (!ci.used_index || cs.used_index) die("unexpected query plan");

        idx_sum += ci.ms;
        if (ci.ms < idx_min) idx_min = ci.ms;
        if (ci.ms > idx_max) idx_max = ci.ms;
        scan_sum += cs.ms;
        if (cs.ms < scan_min) scan_min = cs.ms;
        if (cs.ms > scan_max) scan_max = cs.ms;
        idx_examined = ci.examined;
        scan_examined = cs.examined;
    }
    double idx_avg = idx_sum / queries, scan_avg = scan_sum / queries;
    printf("[3] Point lookup, %d random ids (same row found both ways)\n", queries);
    printf("    %-44s %10s %10s %10s %14s\n", "query", "avg ms", "min ms", "max ms", "rows examined");
    printf("    %-44s %10.4f %10.4f %10.4f %14s\n", "WHERE id = k        (B+ tree index)",
           idx_avg, idx_min, idx_max, commas(idx_examined, a));
    printf("    %-44s %10.2f %10.2f %10.2f %14s\n", "WHERE name = 'user_k' (full scan)",
           scan_avg, scan_min, scan_max, commas(scan_examined, b));
    printf("    => index is about %s times faster\n\n", commas((long long)(scan_avg / (idx_avg > 0 ? idx_avg : 1e-6)), a));

    /* 4) 범위 검색: 리프 연결 리스트 활용 */
    long long from = n > 1000 ? n - 999 : 1;
    Capture cr = {0};
    snprintf(q, sizeof q, "SELECT id FROM users WHERE id >= %lld;", from);
    db_execute_script(db, q, capture, &cr);
    Capture cr2 = {0};
    snprintf(q, sizeof q, "SELECT id FROM users WHERE age = 30;");
    db_execute_script(db, q, capture, &cr2);
    printf("[4] Range / non-key queries\n");
    printf("    WHERE id >= %-10lld %8.3f ms  %s rows  (%s)\n", from, cr.ms, commas(cr.nrows, a), cr.plan);
    printf("    WHERE age = 30          %8.3f ms  %s rows  (%s)\n", cr2.ms, commas(cr2.nrows, b), cr2.plan);

    db_close(db);
    bench_range_ratio(dir, n, batch);
    return 0;
}
