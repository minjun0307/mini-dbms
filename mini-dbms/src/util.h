/*
 * util.h — 프로젝트 전체에서 쓰는 공통 상수와 작은 도구들
 *   - 메모리 할당 래퍼 (실패하면 즉시 종료)
 *   - SBuf: 길이가 자동으로 늘어나는 문자열 버퍼
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdarg.h>
#include <stddef.h>

#define NAME_MAX_LEN 64   /* 테이블/컬럼 이름 최대 길이 (NUL 포함) */
#define MAX_COLUMNS  32   /* 테이블 하나의 최대 컬럼 수 */
#define ERR_MAX      512  /* 에러 메시지 버퍼 크기 */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

double now_ms(void);                         /* 단조 증가 시계 (밀리초) */
int str_ieq(const char *a, const char *b);   /* 대소문자 무시 비교 */
int is_valid_name(const char *s);            /* [A-Za-z_][A-Za-z0-9_]* 이고 길이 제한 이내 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} SBuf;

void sb_init(SBuf *b);
void sb_free(SBuf *b);
void sb_reset(SBuf *b);
void sb_append(SBuf *b, const char *s, size_t n);
void sb_puts(SBuf *b, const char *s);
void sb_putc(SBuf *b, char c);
void sb_printf(SBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
