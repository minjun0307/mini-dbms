/*
 * cli.c — [Phase 1] Command Line Interface
 *
 *   ./build/mdb queries.sql          SQL 파일 실행
 *   ./build/mdb < queries.sql        표준 입력으로 실행
 *   ./build/mdb                      대화형 REPL (mdb> 프롬프트)
 *
 * 옵션: -d DIR  데이터 디렉터리 (기본 ./data)
 *       -q      성공한 INSERT 결과는 출력하지 않음 (대용량 파일용)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "executor.h"

typedef struct {
    int quiet;
    long long statements;
} CliCtx;

static void print_result(const Result *r, void *ctx) {
    CliCtx *c = ctx;
    c->statements++;
    if (!r->ok) {
        fflush(stdout);
        result_print(r, stderr);
        return;
    }
    if (c->quiet && r->type == STMT_INSERT) return;
    result_print(r, stdout);
}

static char *read_all(FILE *fp) {
    SBuf b;
    sb_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) sb_append(&b, chunk, n);
    if (!b.data) sb_append(&b, "", 0);
    return b.data;
}

static void print_tables(DB *db) {
    for (int i = 0; i < db_table_count(db); i++) {
        Table *t = db_table_at(db, i);
        pthread_rwlock_rdlock(&t->lock);
        printf("%s (", t->name);
        for (int c = 0; c < t->schema.ncols; c++)
            printf("%s%s %s", c ? ", " : "", t->schema.cols[c].name, coltype_name(t->schema.cols[c].type));
        printf(")  rows=%lld  index_height=%d\n", t->nrows, t->index->height);
        pthread_rwlock_unlock(&t->lock);
    }
}

static int ends_with_semicolon(const SBuf *b) {
    for (size_t i = b->len; i > 0; i--) {
        char c = b->data[i - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return c == ';';
    }
    return 0;
}

static void repl(DB *db, CliCtx *ctx) {
    printf("mini-dbms REPL. 문장은 ';' 로 끝내세요. 명령: .tables .help .exit\n");
    SBuf buf;
    sb_init(&buf);
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        printf(buf.len ? "   -> " : "mdb> ");
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;
        if (buf.len == 0) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '\n' || *s == '\0') continue;
            if (*s == '.') {
                if (!strncmp(s, ".exit", 5) || !strncmp(s, ".quit", 5)) break;
                if (!strncmp(s, ".tables", 7)) print_tables(db);
                else
                    printf("  INSERT INTO t [(cols)] VALUES (...), (...);\n"
                           "  SELECT * | cols FROM t [WHERE col op value] [LIMIT n];\n"
                           "  op: = != <> < <= > >=   (WHERE id ... 는 B+ 트리 인덱스 사용)\n"
                           "  .tables  테이블 목록   .exit  종료\n");
                continue;
            }
        }
        sb_append(&buf, line, (size_t)n);
        if (ends_with_semicolon(&buf)) {
            db_execute_script(db, buf.data, print_result, ctx);
            sb_reset(&buf);
        }
    }
    if (buf.len) db_execute_script(db, buf.data, print_result, ctx);
    free(line);
    sb_free(&buf);
    printf("\n");
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-d DATA_DIR] [-q] [FILE.sql]\n", prog);
}

int main(int argc, char **argv) {
    const char *dir = "data";
    CliCtx ctx = {0, 0};
    int opt;
    while ((opt = getopt(argc, argv, "d:qh")) != -1) {
        switch (opt) {
        case 'd': dir = optarg; break;
        case 'q': ctx.quiet = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    char err[ERR_MAX];
    DB *db = db_open(dir, err, sizeof err);
    if (!db) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int errors = 0;
    if (optind < argc) {
        FILE *fp = fopen(argv[optind], "rb");
        if (!fp) {
            perror(argv[optind]);
            db_close(db);
            return 1;
        }
        char *sql = read_all(fp);
        fclose(fp);
        double t0 = now_ms();
        errors = db_execute_script(db, sql, print_result, &ctx);
        fprintf(stderr, "-- %lld statement(s), %d error(s), %.1f ms\n", ctx.statements, errors, now_ms() - t0);
        free(sql);
    } else if (!isatty(STDIN_FILENO)) {
        char *sql = read_all(stdin);
        errors = db_execute_script(db, sql, print_result, &ctx);
        free(sql);
    } else {
        repl(db, &ctx);
    }

    db_close(db);
    return errors ? 1 : 0;
}
