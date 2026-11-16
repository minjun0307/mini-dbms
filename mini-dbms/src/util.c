#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void out_of_memory(void) {
    fprintf(stderr, "fatal: out of memory\n");
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int is_valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= NAME_MAX_LEN) return 0;
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') return 0;
    for (size_t i = 1; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}

/* ---------- SBuf ---------- */

void sb_init(SBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(SBuf *b) {
    free(b->data);
    sb_init(b);
}

void sb_reset(SBuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void sb_reserve(SBuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void sb_append(SBuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(SBuf *b, const char *s) { sb_append(b, s, strlen(s)); }

void sb_putc(SBuf *b, char c) { sb_append(b, &c, 1); }

void sb_printf(SBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}
