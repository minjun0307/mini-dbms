/* 엔진 통합 테스트: 실제 임시 디렉터리에 파일을 만들고 SQL 로 검증한다 */
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "executor.h"
#include "test.h"

static char g_dir[64];

static void write_file(const char *name, const char *content, const char *mode) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *fp = fopen(path, mode);
    fputs(content, fp);
    fclose(fp);
}

/* 새 임시 DB: users(id, name, age) + products(id, title, price) */
static DB *fresh_db(void) {
    strcpy(g_dir, "/tmp/mdb_test_XXXXXX");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        exit(1);
    }
    write_file("users.schema", "# test table\nid INT\nname TEXT\nage INT\n", "w");
    write_file("products.schema", "id INT\ntitle TEXT\nprice INT\n", "w");
    char err[ERR_MAX];
    DB *db = db_open(g_dir, err, sizeof err);
    if (!db) fprintf(stderr, "    db_open: %s\n", err);
    return db;
}

static DB *reopen(DB *db) {
    db_close(db);
    char err[ERR_MAX];
    DB *n = db_open(g_dir, err, sizeof err);
    if (!n) fprintf(stderr, "    db_open: %s\n", err);
    return n;
}

static void cleanup(DB *db) {
    db_close(db);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "    cleanup failed\n");
}

/* SQL 한 문장 실행 → Result (호출자가 result_free) */
static int exec1(DB *db, const char *sql, Result *out) {
    Parser p;
    Statement st;
    result_init(out);
    parser_init(&p, sql);
    int rc = parser_next(&p, &st);
    if (rc <= 0) {
        out->ok = 0;
        snprintf(out->error, sizeof out->error, "%s", rc < 0 ? p.err : "no statement");
        return -1;
    }
    db_execute(db, &st, out);
    stmt_free(&st);
    return out->ok ? 0 : -1;
}

static long long count_rows(DB *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT id FROM %s", table);
    Result r;
    exec1(db, sql, &r);
    long long n = r.ok ? r.nrows : -1;
    result_free(&r);
    return n;
}

static void expect_error(DB *db, const char *sql, const char *msg) {
    Result r;
    CHECK_INT(exec1(db, sql, &r), -1);
    CHECK_CONTAINS(r.error, msg);
    result_free(&r);
}

/* ---------- tests ---------- */

static void test_insert_and_select(void) {
    DB *db = fresh_db();
    Result r;
    CHECK_INT(exec1(db, "INSERT INTO users VALUES ('kim', 20)", &r), 0);
    CHECK_INT(r.first_id, 1);
    result_free(&r);
    CHECK_INT(exec1(db, "INSERT INTO users (age, name) VALUES (25, 'lee'), (30, 'park')", &r), 0);
    CHECK_INT(r.inserted, 2);
    CHECK_INT(r.first_id, 2);
    CHECK_INT(r.last_id, 3);
    result_free(&r);

    CHECK_INT(exec1(db, "SELECT * FROM users", &r), 0);
    CHECK_INT(r.nrows, 3);
    CHECK_INT(r.ncols, 3);
    CHECK_STR(r.colnames[1], "name");
    CHECK_INT(r.cells[1 * 3 + 0].i, 2);
    CHECK_STR(r.cells[1 * 3 + 1].s, "lee");
    CHECK_INT(r.cells[1 * 3 + 2].i, 25);
    result_free(&r);

    CHECK_INT(exec1(db, "select AGE, Name from USERS where NAME = 'park'", &r), 0);  /* 이름은 대소문자 무시 */
    CHECK_INT(r.nrows, 1);
    CHECK_INT(r.cells[0].i, 30);
    CHECK_STR(r.cells[1].s, "park");
    result_free(&r);

    CHECK_INT(count_rows(db, "products"), 0);  /* 빈 테이블 */
    cleanup(db);
}

static void test_semantic_errors(void) {
    DB *db = fresh_db();
    expect_error(db, "INSERT INTO nope VALUES (1)", "table 'nope' does not exist");
    expect_error(db, "INSERT INTO users VALUES ('kim')", "expects 2 values (name, age) but got 1");
    expect_error(db, "INSERT INTO users VALUES ('kim', 1, 2)", "expects 2 values");
    expect_error(db, "INSERT INTO users VALUES (20, 'kim')", "column 'name' is TEXT but got integer 20");
    expect_error(db, "INSERT INTO users VALUES ('kim', 'abc')", "column 'age' is INT but got string 'abc'");
    expect_error(db, "INSERT INTO users (id, name, age) VALUES (1, 'a', 2)", "assigned automatically");
    expect_error(db, "INSERT INTO users (name) VALUES ('a')", "missing value for column 'age'");
    expect_error(db, "INSERT INTO users (name, name) VALUES ('a', 'b')", "listed more than once");
    expect_error(db, "INSERT INTO users (name, foo) VALUES ('a', 1)", "unknown column 'foo'");
    expect_error(db, "SELECT foo FROM users", "unknown column 'foo'");
    expect_error(db, "SELECT * FROM users WHERE foo = 1", "unknown column 'foo' in WHERE");
    expect_error(db, "SELECT * FROM users WHERE id = 'x'", "is INT but was compared with string");
    expect_error(db, "SELECT * FROM users WHERE name = 3", "is TEXT but was compared with integer");
    CHECK_INT(count_rows(db, "users"), 0);
    cleanup(db);
}

static void test_multi_row_insert_is_atomic(void) {
    DB *db = fresh_db();
    Result r;
    exec1(db, "INSERT INTO users VALUES ('a', 1)", &r);
    result_free(&r);
    /* 3번째 행의 타입이 틀림 → 앞의 두 행도 들어가면 안 된다 */
    expect_error(db, "INSERT INTO users VALUES ('b', 2), ('c', 3), ('d', 'x')", "row 3");
    CHECK_INT(count_rows(db, "users"), 1);
    /* 다음 id 도 건너뛰지 않는다 */
    exec1(db, "INSERT INTO users VALUES ('e', 5)", &r);
    CHECK_INT(r.first_id, 2);
    result_free(&r);
    cleanup(db);
}

static void test_special_strings_roundtrip(void) {
    DB *db = fresh_db();
    const char *names[] = {"kim, jr", "O'Brien", "say \"hi\"", "민수", "", "a,\"b\",c", "  spaces  "};
    int n = (int)(sizeof names / sizeof names[0]);
    char sql[256];
    Result r;
    CHECK_INT(exec1(db, "INSERT INTO users VALUES ('kim, jr', 1), ('O''Brien', 2), ('say \"hi\"', 3), "
                        "('민수', 4), ('', 5), ('a,\"b\",c', 6), ('  spaces  ', 7)", &r), 0);
    result_free(&r);
    db = reopen(db);  /* 파일에서 다시 읽어도 똑같아야 한다 */
    for (int i = 0; i < n; i++) {
        snprintf(sql, sizeof sql, "SELECT name FROM users WHERE id = %d", i + 1);
        CHECK_INT(exec1(db, sql, &r), 0);
        CHECK_INT(r.nrows, 1);
        if (r.nrows == 1) CHECK_STR(r.cells[0].s, names[i]);
        result_free(&r);
    }
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE name = 'kim, jr'", &r), 0);
    CHECK_INT(r.nrows, 1);
    result_free(&r);
    expect_error(db, "INSERT INTO users VALUES ('line\nbreak', 1)", "line breaks are not allowed");
    cleanup(db);
}

static void test_index_matches_full_scan(void) {
    /* 같은 조건을 인덱스로 찾은 결과와 직접 필터링한 결과가 같아야 한다 */
    DB *db = fresh_db();
    SBuf sql;
    sb_init(&sql);
    sb_puts(&sql, "INSERT INTO users VALUES ");
    for (int i = 1; i <= 300; i++) sb_printf(&sql, "%s('u%d', %d)", i > 1 ? ", " : "", i, i % 17);
    Result r;
    CHECK_INT(exec1(db, sql.data, &r), 0);
    result_free(&r);
    sb_free(&sql);

    const char *ops[] = {"=", "<", "<=", ">", ">=", "!="};
    long long ks[] = {-5, 1, 150, 300, 301};
    for (int o = 0; o < 6; o++) {
        for (int k = 0; k < 5; k++) {
            char q[128];
            snprintf(q, sizeof q, "SELECT id FROM users WHERE id %s %lld", ops[o], ks[k]);
            CHECK_INT(exec1(db, q, &r), 0);
            long long expected = 0;
            for (long long id = 1; id <= 300; id++) {
                long long key = ks[k];
                int m = o == 0 ? id == key : o == 1 ? id < key : o == 2 ? id <= key
                      : o == 3 ? id > key : o == 4 ? id >= key : id != key;
                expected += m;
            }
            CHECK_INT(r.nrows, expected);
            /* = 는 항상 인덱스, != 는 항상 풀 스캔,
             * 범위 조건은 결과가 전체의 index_range_max_ratio 이하일 때만 인덱스 */
            int want_index = o == 0 || (o != 5 && (double)expected / 300 <= index_range_max_ratio);
            CHECK_INT(r.used_index, want_index);
            int sorted = 1;
            for (long long i = 1; i < r.nrows; i++)
                if (r.cells[i].i <= r.cells[i - 1].i) sorted = 0;
            CHECK(sorted);
            result_free(&r);
        }
    }

    CHECK_INT(exec1(db, "SELECT * FROM users WHERE id = 123", &r), 0);
    CHECK_INT(r.examined, 1);  /* 인덱스: 딱 1행만 읽음 */
    CHECK_CONTAINS(r.plan, "INDEX LOOKUP");
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT * FROM users WHERE name = 'u123'", &r), 0);
    CHECK_INT(r.nrows, 1);
    CHECK_INT(r.examined, 300);  /* 풀 스캔: 전부 읽음 */
    CHECK_CONTAINS(r.plan, "FULL SCAN");
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE age = 0", &r), 0);
    CHECK_INT(r.nrows, 17);  /* 17, 34, ..., 289 */
    result_free(&r);
    cleanup(db);
}

static void test_range_plan_by_ratio(void) {
    /* 범위 조건: 예상 결과 비율이 기준(14%) 이하이면 인덱스, 넘으면 풀 스캔. 결과는 같아야 한다 */
    DB *db = fresh_db();
    SBuf sql;
    sb_init(&sql);
    sb_puts(&sql, "INSERT INTO users VALUES ");
    for (int i = 1; i <= 1000; i++) sb_printf(&sql, "%s('u%d', %d)", i > 1 ? ", " : "", i, i % 50);
    Result r;
    CHECK_INT(exec1(db, sql.data, &r), 0);
    result_free(&r);
    sb_free(&sql);

    /* 1000 행 중 140 행(14%) → 기준 이하 → 인덱스 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id > 860", &r), 0);
    CHECK_INT(r.nrows, 140);
    CHECK_INT(r.used_index, 1);
    CHECK_INT(r.examined, 140);
    CHECK_CONTAINS(r.plan, "INDEX RANGE SCAN");
    CHECK_CONTAINS(r.plan, "est. 14.0%");
    result_free(&r);

    /* 141 행(14.1%) → 기준 초과 → 풀 스캔 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id >= 860", &r), 0);
    CHECK_INT(r.nrows, 141);
    CHECK_INT(r.used_index, 0);
    CHECK_INT(r.examined, 1000);
    CHECK_CONTAINS(r.plan, "FULL SCAN");
    CHECK_CONTAINS(r.plan, "est. 14.1%");
    if (r.nrows == 141) CHECK_INT(r.cells[0].i, 860);
    result_free(&r);

    /* < 와 <= 도 같은 기준: 1..140 은 인덱스, 1..141 은 풀 스캔 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id <= 140", &r), 0);
    CHECK_INT(r.nrows, 140);
    CHECK_INT(r.used_index, 1);
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id < 142", &r), 0);
    CHECK_INT(r.nrows, 141);
    CHECK_INT(r.used_index, 0);
    result_free(&r);

    /* 전체가 나오는 범위라도 LIMIT 이 작으면 인덱스가 그만큼만 읽고 멈춘다 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id >= 1 LIMIT 10", &r), 0);
    CHECK_INT(r.nrows, 10);
    CHECK_INT(r.used_index, 1);
    CHECK_INT(r.examined, 10);
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id >= 1", &r), 0);
    CHECK_INT(r.nrows, 1000);
    CHECK_INT(r.used_index, 0);
    result_free(&r);

    /* 범위 밖 → 예상 0 행 → 인덱스로 바로 끝 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id > 1000", &r), 0);
    CHECK_INT(r.nrows, 0);
    CHECK_INT(r.used_index, 1);
    CHECK_INT(r.examined, 0);
    result_free(&r);

    /* = 는 비율과 상관없이 항상 인덱스 */
    CHECK_INT(exec1(db, "SELECT id FROM users WHERE id = 500", &r), 0);
    CHECK_INT(r.used_index, 1);
    CHECK_CONTAINS(r.plan, "INDEX LOOKUP");
    result_free(&r);
    cleanup(db);
}

static void test_limit(void) {
    DB *db = fresh_db();
    Result r;
    exec1(db, "INSERT INTO users VALUES ('a',1),('b',2),('c',3),('d',4),('e',5)", &r);
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT * FROM users LIMIT 2", &r), 0);
    CHECK_INT(r.nrows, 2);
    CHECK_INT(r.examined, 2);  /* 2개 찾으면 바로 멈춤 */
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT * FROM users WHERE id > 1 LIMIT 3", &r), 0);
    CHECK_INT(r.nrows, 3);
    CHECK_INT(r.cells[0].i, 2);
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT * FROM users WHERE id = 1 LIMIT 0", &r), 0);
    CHECK_INT(r.nrows, 0);
    result_free(&r);
    cleanup(db);
}

static void test_persistence_and_next_id(void) {
    DB *db = fresh_db();
    Result r;
    exec1(db, "INSERT INTO users VALUES ('a', 1), ('b', 2)", &r);
    result_free(&r);
    exec1(db, "INSERT INTO users VALUES ('c', 3)", &r);
    result_free(&r);
    db = reopen(db);
    CHECK_INT(count_rows(db, "users"), 3);
    CHECK_INT(exec1(db, "INSERT INTO users VALUES ('d', 4)", &r), 0);
    CHECK_INT(r.first_id, 4);  /* 재시작 후에도 id 가 이어진다 */
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT name FROM users WHERE id = 3", &r), 0);
    CHECK_STR(r.nrows ? r.cells[0].s : "", "c");
    result_free(&r);
    cleanup(db);
}

static void test_recovery_partial_last_line(void) {
    DB *db = fresh_db();
    Result r;
    exec1(db, "INSERT INTO users VALUES ('a', 1), ('b', 2)", &r);
    result_free(&r);
    db_close(db);
    /* 크래시 흉내: 마지막 줄을 쓰다가 멈춤 ('\n' 없음) */
    write_file("users.tbl", "3,\"bro", "a");
    char err[ERR_MAX];
    db = db_open(g_dir, err, sizeof err);
    CHECK(db != NULL);
    Table *t = db_find_table(db, "users");
    CHECK(t->recovered_bytes == 6);
    CHECK_INT(count_rows(db, "users"), 2);
    CHECK_INT(exec1(db, "INSERT INTO users VALUES ('c', 3)", &r), 0);
    CHECK_INT(r.first_id, 3);
    result_free(&r);
    CHECK_INT(exec1(db, "SELECT name FROM users WHERE id = 3", &r), 0);
    CHECK_STR(r.nrows ? r.cells[0].s : "", "c");
    result_free(&r);
    cleanup(db);
}

static void test_recovery_incomplete_batch(void) {
    DB *db = fresh_db();
    Result r;
    exec1(db, "INSERT INTO users VALUES ('a', 1)", &r);
    result_free(&r);
    db_close(db);
    /* 크래시 흉내: 3행짜리 묶음 중 2행만 완전히 쓰임 → 묶음 전체를 버려야 한다 */
    write_file("users.tbl", "@B 3\n2,\"b\",2\n3,\"c\",3\n", "a");
    char err[ERR_MAX];
    db = db_open(g_dir, err, sizeof err);
    CHECK(db != NULL);
    CHECK(db_find_table(db, "users")->recovered_bytes > 0);
    CHECK_INT(count_rows(db, "users"), 1);
    CHECK_INT(exec1(db, "SELECT * FROM users WHERE id = 2", &r), 0);
    CHECK_INT(r.nrows, 0);
    result_free(&r);
    db = reopen(db);  /* 복구는 한 번만: 두 번째로 열 때는 잘라낼 게 없다 */
    CHECK_INT(db_find_table(db, "users")->recovered_bytes, 0);
    CHECK_INT(count_rows(db, "users"), 1);
    cleanup(db);
}

static void test_corrupted_file_is_rejected(void) {
    DB *db = fresh_db();
    db_close(db);
    write_file("users.tbl", "1,\"a\",1\nGARBAGE\n", "w");
    char err[ERR_MAX];
    db = db_open(g_dir, err, sizeof err);
    CHECK(db == NULL);
    CHECK_CONTAINS(err, "corrupted row");
    db = NULL;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "    cleanup failed\n");
}

static void test_script_continues_after_error(void) {
    DB *db = fresh_db();
    int errors = db_execute_script(db,
        "INSERT INTO users VALUES ('a', 1);\n"
        "INSERT INTO users VALUES (oops);\n"
        "SELECT * FROM missing;\n"
        "INSERT INTO users VALUES ('b', 2);\n", NULL, NULL);
    CHECK_INT(errors, 2);
    CHECK_INT(count_rows(db, "users"), 2);
    cleanup(db);
}

/* ---------- 동시성 ---------- */

#define WRITERS 8
#define READERS 4
#define INSERTS_PER_WRITER 200

static DB *g_cdb;
static volatile int g_writers_done;
static int g_thread_errors;
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;

static void thread_error(const char *msg) {
    pthread_mutex_lock(&g_err_mu);
    if (g_thread_errors++ < 3) fprintf(stderr, "    thread error: %s\n", msg);
    pthread_mutex_unlock(&g_err_mu);
}

static void *writer(void *arg) {
    long w = (long)arg;
    char sql[128];
    for (int i = 0; i < INSERTS_PER_WRITER; i++) {
        Result r;
        if (i % 10 == 0)
            snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('w%ld_%d', %d), ('w%ld_%db', %d)", w, i, i, w, i, i);
        else
            snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('w%ld_%d', %d)", w, i, i);
        if (exec1(g_cdb, sql, &r) != 0) thread_error(r.error);
        result_free(&r);
    }
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    while (!g_writers_done) {
        Result r;
        if (exec1(g_cdb, "SELECT * FROM users WHERE id >= 1", &r) != 0) thread_error(r.error);
        /* 읽는 도중 묶음 INSERT 의 일부만 보이면 안 된다: id 가 1..n 으로 빈틈없이 이어져야 함 */
        for (long long i = 0; i < r.nrows; i++)
            if (r.cells[i * r.ncols].i != i + 1) {
                thread_error("reader saw a gap in ids (partial write visible)");
                break;
            }
        result_free(&r);
        if (exec1(g_cdb, "SELECT name FROM users WHERE age = 7", &r) != 0) thread_error(r.error);
        result_free(&r);
    }
    return NULL;
}

static void test_concurrent_readers_and_writers(void) {
    g_cdb = fresh_db();
    g_writers_done = 0;
    g_thread_errors = 0;
    pthread_t w[WRITERS], rd[READERS];
    for (long i = 0; i < READERS; i++) pthread_create(&rd[i], NULL, reader, NULL);
    for (long i = 0; i < WRITERS; i++) pthread_create(&w[i], NULL, writer, (void *)i);
    for (int i = 0; i < WRITERS; i++) pthread_join(w[i], NULL);
    g_writers_done = 1;
    for (int i = 0; i < READERS; i++) pthread_join(rd[i], NULL);

    CHECK_INT(g_thread_errors, 0);
    long long expected = WRITERS * (INSERTS_PER_WRITER + INSERTS_PER_WRITER / 10);
    Result r;
    CHECK_INT(exec1(g_cdb, "SELECT id FROM users", &r), 0);
    CHECK_INT(r.nrows, expected);
    int contiguous = 1;
    for (long long i = 0; i < r.nrows; i++)
        if (r.cells[i].i != i + 1) contiguous = 0;
    CHECK(contiguous);  /* id 중복도 빈틈도 없음 */
    result_free(&r);

    Table *t = db_find_table(g_cdb, "users");
    char err[256];
    CHECK(bpt_check(t->index, err, sizeof err));
    CHECK_INT((long long)t->index->size, expected);

    g_cdb = reopen(g_cdb);  /* 파일 내용도 일관성이 있어야 한다 */
    CHECK_INT(count_rows(g_cdb, "users"), expected);
    cleanup(g_cdb);
}

void engine_tests(void) {
    RUN_TEST(test_insert_and_select);
    RUN_TEST(test_semantic_errors);
    RUN_TEST(test_multi_row_insert_is_atomic);
    RUN_TEST(test_special_strings_roundtrip);
    RUN_TEST(test_index_matches_full_scan);
    RUN_TEST(test_range_plan_by_ratio);
    RUN_TEST(test_limit);
    RUN_TEST(test_persistence_and_next_id);
    RUN_TEST(test_recovery_partial_last_line);
    RUN_TEST(test_recovery_incomplete_batch);
    RUN_TEST(test_corrupted_file_is_rejected);
    RUN_TEST(test_script_continues_after_error);
    RUN_TEST(test_concurrent_readers_and_writers);
}
