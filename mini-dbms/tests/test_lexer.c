#include "lexer.h"
#include "test.h"

typedef struct {
    TokenType type;
    const char *text;   /* NULL 이면 텍스트는 검사하지 않음 */
} Exp;

static void expect_tokens(const char *src, const Exp *exp, int n, int line) {
    Lexer lx;
    lexer_init(&lx, src);
    for (int i = 0; i < n; i++) {
        Token t = lexer_next(&lx);
        g_checks++;
        if (t.type != exp[i].type || (exp[i].text && strcmp(t.text, exp[i].text) != 0)) {
            g_failures++;
            fprintf(stderr, "    FAIL test_lexer.c:%d: input \"%s\" token #%d: got (%s, \"%s\"), expected (%s, \"%s\")\n",
                    line, src, i, token_type_name(t.type), t.text, token_type_name(exp[i].type),
                    exp[i].text ? exp[i].text : "*");
            return;
        }
    }
}

#define EXPECT_TOKENS(src, ...)                                              \
    do {                                                                     \
        Exp _e[] = {__VA_ARGS__};                                            \
        expect_tokens(src, _e, (int)(sizeof _e / sizeof _e[0]), __LINE__);   \
    } while (0)

static void test_basic_select(void) {
    EXPECT_TOKENS("SELECT * FROM users;",
                  {TOK_KEYWORD, "SELECT"}, {TOK_STAR, "*"}, {TOK_KEYWORD, "FROM"},
                  {TOK_IDENT, "users"}, {TOK_SEMICOLON, ";"}, {TOK_EOF, ""});
}

static void test_keywords_are_case_insensitive(void) {
    EXPECT_TOKENS("select SeLeCt insert", {TOK_KEYWORD, "SELECT"}, {TOK_KEYWORD, "SELECT"},
                  {TOK_KEYWORD, "INSERT"});
}

static void test_keyword_prefix_is_identifier(void) {
    /* 단어를 끝까지 읽은 뒤 비교하므로 select + ed 로 잘리지 않는다 */
    EXPECT_TOKENS("selected from_date", {TOK_IDENT, "selected"}, {TOK_IDENT, "from_date"});
}

static void test_whitespace_and_no_spaces(void) {
    EXPECT_TOKENS("SELECT\t\n  *", {TOK_KEYWORD, "SELECT"}, {TOK_STAR, "*"}, {TOK_EOF, ""});
    EXPECT_TOKENS("VALUES(1,'a')", {TOK_KEYWORD, "VALUES"}, {TOK_LPAREN, "("}, {TOK_INT, "1"},
                  {TOK_COMMA, ","}, {TOK_STRING, "a"}, {TOK_RPAREN, ")"}, {TOK_EOF, ""});
}

static void test_string_keeps_commas_and_spaces(void) {
    EXPECT_TOKENS("'kim, lee'", {TOK_STRING, "kim, lee"}, {TOK_EOF, ""});
    EXPECT_TOKENS("'hello world; DROP'", {TOK_STRING, "hello world; DROP"}, {TOK_EOF, ""});
}

static void test_string_escape_and_empty(void) {
    EXPECT_TOKENS("'O''Brien'", {TOK_STRING, "O'Brien"});
    EXPECT_TOKENS("''", {TOK_STRING, ""}, {TOK_EOF, ""});
    EXPECT_TOKENS("''''", {TOK_STRING, "'"}, {TOK_EOF, ""});
}

static void test_unicode_string(void) {
    EXPECT_TOKENS("'민수'", {TOK_STRING, "민수"}, {TOK_EOF, ""});
}

static void test_unterminated_string(void) {
    EXPECT_TOKENS("'kim", {TOK_ERROR, NULL});
}

static void test_string_too_long(void) {
    char src[400];
    src[0] = '\'';
    memset(src + 1, 'a', 300);
    src[301] = '\'';
    src[302] = '\0';
    EXPECT_TOKENS(src, {TOK_ERROR, NULL}, {TOK_EOF, ""});  /* 에러 후에도 위치가 맞춰져 있다 */
}

static void test_numbers(void) {
    EXPECT_TOKENS("42 -7 0", {TOK_INT, "42"}, {TOK_INT, "-7"}, {TOK_INT, "0"});
    EXPECT_TOKENS("12abc", {TOK_ERROR, NULL});
}

static void test_operators(void) {
    EXPECT_TOKENS("= != <> < <= > >=", {TOK_EQ, "="}, {TOK_NE, "!="}, {TOK_NE, "<>"}, {TOK_LT, "<"},
                  {TOK_LE, "<="}, {TOK_GT, ">"}, {TOK_GE, ">="}, {TOK_EOF, ""});
}

static void test_invalid_characters(void) {
    EXPECT_TOKENS("SELECT @", {TOK_KEYWORD, "SELECT"}, {TOK_ERROR, NULL});
    EXPECT_TOKENS("!", {TOK_ERROR, NULL});
}

static void test_comments(void) {
    EXPECT_TOKENS("-- hello\nSELECT -- trailing\n*", {TOK_KEYWORD, "SELECT"}, {TOK_STAR, "*"}, {TOK_EOF, ""});
}

static void test_empty_input(void) {
    EXPECT_TOKENS("", {TOK_EOF, ""}, {TOK_EOF, ""});  /* EOF 이후에도 계속 EOF */
    EXPECT_TOKENS("   \n\t ", {TOK_EOF, ""});
}

static void test_line_and_column(void) {
    Lexer lx;
    lexer_init(&lx, "SELECT\n  *");
    Token a = lexer_next(&lx);
    Token b = lexer_next(&lx);
    CHECK_INT(a.line, 1);
    CHECK_INT(a.col, 1);
    CHECK_INT(b.line, 2);
    CHECK_INT(b.col, 3);
}

void lexer_tests(void) {
    RUN_TEST(test_basic_select);
    RUN_TEST(test_keywords_are_case_insensitive);
    RUN_TEST(test_keyword_prefix_is_identifier);
    RUN_TEST(test_whitespace_and_no_spaces);
    RUN_TEST(test_string_keeps_commas_and_spaces);
    RUN_TEST(test_string_escape_and_empty);
    RUN_TEST(test_unicode_string);
    RUN_TEST(test_unterminated_string);
    RUN_TEST(test_string_too_long);
    RUN_TEST(test_numbers);
    RUN_TEST(test_operators);
    RUN_TEST(test_invalid_characters);
    RUN_TEST(test_comments);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_line_and_column);
}
