/* 아주 작은 단위 테스트 매크로 모음 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                        \
    } while (0)

#define CHECK_INT(actual, expected)                                                       \
    do {                                                                                  \
        long long _a = (long long)(actual), _e = (long long)(expected);                   \
        g_checks++;                                                                       \
        if (_a != _e) {                                                                   \
            g_failures++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,      \
                    __LINE__, #actual, _a, _e);                                           \
        }                                                                                 \
    } while (0)

#define CHECK_STR(actual, expected)                                                       \
    do {                                                                                  \
        const char *_a = (actual), *_e = (expected);                                      \
        g_checks++;                                                                       \
        if (!_a || strcmp(_a, _e) != 0) {                                                 \
            g_failures++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__,  \
                    __LINE__, #actual, _a ? _a : "(null)", _e);                           \
        }                                                                                 \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                                  \
    do {                                                                                  \
        const char *_h = (haystack), *_n = (needle);                                      \
        g_checks++;                                                                       \
        if (!_h || !strstr(_h, _n)) {                                                     \
            g_failures++;                                                                 \
            fprintf(stderr, "    FAIL %s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, \
                    __LINE__, _h ? _h : "(null)", _n);                                    \
        }                                                                                 \
    } while (0)

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        int _before = g_failures;                                          \
        fn();                                                              \
        printf("  %s %s\n", g_failures == _before ? "PASS" : "FAIL", #fn); \
    } while (0)

void lexer_tests(void);
void parser_tests(void);
void bptree_tests(void);
void engine_tests(void);

#endif
