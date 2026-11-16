#include "parser.h"
#include "test.h"

static int parse_one(const char *sql, Statement *st, char *err) {
    Parser p;
    parser_init(&p, sql);
    int rc = parser_next(&p, st);
    if (err) strcpy(err, p.err);
    return rc;
}

static void test_insert_without_columns(void) {
    Statement st;
    CHECK_INT(parse_one("INSERT INTO users VALUES ('kim', 20);", &st, NULL), 1);
    CHECK_INT(st.type, STMT_INSERT);
    CHECK_STR(st.table, "users");
    CHECK_INT(st.ncols, 0);
    CHECK_INT(st.nrows, 1);
    CHECK_INT(st.nvals, 2);
    CHECK_INT(st.values[0].type, VAL_TEXT);
    CHECK_STR(st.values[0].s, "kim");
    CHECK_INT(st.values[1].type, VAL_INT);
    CHECK_INT(st.values[1].i, 20);
    stmt_free(&st);
}

static void test_insert_with_columns_and_rows(void) {
    Statement st;
    CHECK_INT(parse_one("insert into users (age, name) values (20, 'a'), (30, 'b'), (-1, 'c')", &st, NULL), 1);
    CHECK_INT(st.ncols, 2);
    CHECK_STR(st.cols[0], "age");
    CHECK_STR(st.cols[1], "name");
    CHECK_INT(st.nrows, 3);
    CHECK_INT(st.values[2 * 2 + 0].i, -1);
    CHECK_STR(st.values[2 * 2 + 1].s, "c");
    stmt_free(&st);
}

static void test_select_variants(void) {
    Statement st;
    CHECK_INT(parse_one("SELECT * FROM users", &st, NULL), 1);
    CHECK_INT(st.select_all, 1);
    CHECK_INT(st.has_where, 0);
    CHECK_INT(st.limit, -1);
    stmt_free(&st);

    CHECK_INT(parse_one("SELECT name, age FROM users WHERE id >= 10 LIMIT 5;", &st, NULL), 1);
    CHECK_INT(st.select_all, 0);
    CHECK_INT(st.nproj, 2);
    CHECK_STR(st.proj[1], "age");
    CHECK_INT(st.has_where, 1);
    CHECK_STR(st.where_col, "id");
    CHECK_INT(st.where_op, OP_GE);
    CHECK_INT(st.where_val.i, 10);
    CHECK_INT(st.limit, 5);
    stmt_free(&st);

    CHECK_INT(parse_one("SELECT id FROM users WHERE name = 'O''Brien'", &st, NULL), 1);
    CHECK_STR(st.where_val.s, "O'Brien");
    stmt_free(&st);

    CHECK_INT(parse_one("SELECT id FROM users WHERE age <> 3", &st, NULL), 1);
    CHECK_INT(st.where_op, OP_NE);
    stmt_free(&st);
}

static void test_syntax_errors(void) {
    struct {
        const char *sql;
        const char *msg;
    } cases[] = {
        {"INSERT users VALUES (1)", "expected keyword INTO"},
        {"INSERT INTO users (1)", "expected column name"},
        {"INSERT INTO users VALUES 1", "expected '('"},
        {"INSERT INTO users VALUES (1,)", "expected a value"},
        {"INSERT INTO users VALUES (1, 2), (3)", "row 2 has 1 values but row 1 has 2"},
        {"SELECT FROM users", "expected column name or '*'"},
        {"SELECT * users", "expected keyword FROM"},
        {"SELECT * FROM users WHERE id 3", "expected comparison operator"},
        {"SELECT * FROM users WHERE id = name", "expected a value"},
        {"SELECT * FROM users LIMIT -1", "LIMIT expects a non-negative integer"},
        {"SELECT * FROM a SELECT * FROM b", "expected ';'"},
        {"DELETE FROM users", "expected INSERT or SELECT"},
        {"SELECT * FROM users WHERE name = 'abc", "unterminated string"},
        {"INSERT INTO t VALUES (99999999999999999999)", "integer out of range"},
        {"SELECT * FROM users WHERE id = 1 @", "unexpected character '@'"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Statement st;
        char err[ERR_MAX];
        int rc = parse_one(cases[i].sql, &st, err);
        CHECK_INT(rc, -1);
        CHECK_CONTAINS(err, cases[i].msg);
    }
}

static void test_error_has_position(void) {
    Statement st;
    char err[ERR_MAX];
    CHECK_INT(parse_one("SELECT *\nFROM users\nWHERE id ! 3", &st, err), -1);
    CHECK_CONTAINS(err, "line 3");
}

static void test_multiple_statements_and_recovery(void) {
    /* 두 번째 문장이 틀려도 세 번째 문장은 계속 파싱된다 */
    Parser p;
    Statement st;
    parser_init(&p, ";; SELECT * FROM a; INSERT users garbage 'x;y' ; SELECT id FROM b;;");
    CHECK_INT(parser_next(&p, &st), 1);
    CHECK_STR(st.table, "a");
    stmt_free(&st);
    CHECK_INT(parser_next(&p, &st), -1);
    CHECK_INT(parser_next(&p, &st), 1);
    CHECK_STR(st.table, "b");
    stmt_free(&st);
    CHECK_INT(parser_next(&p, &st), 0);
    CHECK_INT(parser_next(&p, &st), 0);
}

static void test_empty_input(void) {
    Statement st;
    CHECK_INT(parse_one("", &st, NULL), 0);
    CHECK_INT(parse_one("  ;;; -- only a comment", &st, NULL), 0);
}

void parser_tests(void) {
    RUN_TEST(test_insert_without_columns);
    RUN_TEST(test_insert_with_columns_and_rows);
    RUN_TEST(test_select_variants);
    RUN_TEST(test_syntax_errors);
    RUN_TEST(test_error_has_position);
    RUN_TEST(test_multiple_statements_and_recovery);
    RUN_TEST(test_empty_input);
}
