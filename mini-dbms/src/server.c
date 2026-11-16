/*
 * server.c — [Phase 3] 미니 DBMS API 서버 (HTTP/1.1 + 스레드 풀)
 *
 *   ./build/mdb_server [-d DIR] [-p PORT] [-t THREADS] [-q QUEUE]
 *
 * 구조
 *   main 스레드:  accept() 만 반복 → 연결(fd)을 스레드 풀 큐에 넣는다
 *   워커 스레드:  HTTP 요청 읽기 → SQL 실행(db_execute_script) → JSON 응답 → 연결 닫기
 *
 * API
 *   POST /query    body = SQL 텍스트 (여러 문장 가능)  → 문장별 결과 JSON
 *   GET  /health   → {"ok":true}
 *   GET  /stats    → 스레드 풀 / 테이블 상태
 *
 * 동시성 경계
 *   - Lexer, Parser, Result 는 요청마다 새로 만든다 → 공유 상태 없음
 *   - 테이블 목록은 시작할 때 만들고 바뀌지 않음 → 락 불필요
 *   - 테이블 데이터(파일 + B+ 트리)는 테이블별 rwlock 으로 보호 (executor.c)
 *       SELECT 끼리는 동시에, INSERT 는 혼자서
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "executor.h"
#include "threadpool.h"

#define MAX_HEADER_BYTES 8192
#define MAX_BODY_BYTES (8 * 1024 * 1024)
#define SOCKET_TIMEOUT_SEC 5

static volatile sig_atomic_t g_stop = 0;
static DB *g_db;
static ThreadPool *g_pool;
static double g_started_at;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    int fd;
    double accepted_at;
} Conn;

typedef struct {
    char method[16];
    char path[256];
    char *body;
    size_t body_len;
} Request;

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int status, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "X-Worker: %d\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason_phrase(status), len, tp_worker_id());
    if (send_all(fd, head, (size_t)n) == 0) send_all(fd, body, len);
}

static void send_error(int fd, int status, const char *msg) {
    char body[512];
    int n = snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", msg);
    send_response(fd, status, body, (size_t)n);
}

/* 클라이언트가 보낸 데이터를 다 읽지 않은 채 close() 하면 커널이 FIN 대신 RST 를 보내고,
 * 클라이언트는 우리가 보낸 에러 응답을 읽기도 전에 "connection reset" 을 받는다.
 * 그래서 쓰기 방향만 먼저 닫고(FIN), 남은 입력을 잠깐 비운 뒤 닫는다. */
static void close_after_error(int fd) {
    shutdown(fd, SHUT_WR);
    struct timeval tv = {0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[4096];
    size_t total = 0;
    while (total < 256 * 1024) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
}

/* HTTP 요청 하나를 읽는다. 0 = 성공, >0 = 돌려줄 에러 상태 코드, -1 = 응답 없이 닫기 */
static int read_request(int fd, Request *req) {
    char hdr[MAX_HEADER_BYTES + 1];
    size_t len = 0;
    char *end = NULL;
    while (!end) {
        if (len >= MAX_HEADER_BYTES) return 431;
        ssize_t n = recv(fd, hdr + len, MAX_HEADER_BYTES - len, 0);
        if (n == 0) return -1;  /* 클라이언트가 그냥 끊음 */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;          /* 타임아웃 등 */
        }
        len += (size_t)n;
        hdr[len] = '\0';
        end = strstr(hdr, "\r\n\r\n");
    }
    size_t header_len = (size_t)(end - hdr) + 4;

    if (sscanf(hdr, "%15s %255s", req->method, req->path) != 2) return 400;

    /* 헤더에서 Content-Length 찾기 */
    long long content_length = -1;
    char *line = strstr(hdr, "\r\n");
    while (line && line < end) {
        line += 2;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *v = line + 15, *endp;
            while (*v == ' ' || *v == '\t') v++;
            errno = 0;
            content_length = strtoll(v, &endp, 10);
            if (endp == v || errno != 0 || content_length < 0) return 400;
        }
        line = strstr(line, "\r\n");
    }

    if (strcmp(req->method, "POST") == 0 && content_length < 0) return 411;
    if (content_length < 0) content_length = 0;
    if (content_length > MAX_BODY_BYTES) return 413;

    /* 본문: 헤더와 같이 들어온 부분 + 나머지를 더 받는다 */
    size_t cl = (size_t)content_length;
    req->body = xmalloc(cl + 1);
    size_t have = len - header_len;
    if (have > cl) have = cl;
    memcpy(req->body, hdr + header_len, have);
    while (have < cl) {
        ssize_t n = recv(fd, req->body + have, cl - have, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    req->body[cl] = '\0';
    req->body_len = cl;
    return 0;
}

typedef struct {
    SBuf *out;
    int count;
    int errors;
} JsonCtx;

static void append_json_result(const Result *r, void *ctx) {
    JsonCtx *c = ctx;
    if (c->count++) sb_putc(c->out, ',');
    result_to_json(r, c->out);
    if (!r->ok) c->errors++;
}

static int is_blank(const char *s) {
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 0;
    return 1;
}

static int handle_query(int fd, const Request *req) {
    if (strcmp(req->method, "POST") != 0) {
        send_error(fd, 405, "use POST /query with SQL in the request body");
        return 405;
    }
    if (is_blank(req->body)) {
        send_error(fd, 400, "empty SQL");
        return 400;
    }
    SBuf results, out;
    sb_init(&results);
    sb_init(&out);
    JsonCtx ctx = {&results, 0, 0};
    db_execute_script(g_db, req->body, append_json_result, &ctx);  /* ← Phase 1+2 엔진 */

    int status = (ctx.count == 0 || ctx.errors) ? 400 : 200;
    sb_printf(&out, "{\"ok\":%s,\"statements\":%d,\"errors\":%d,\"results\":[",
              status == 200 ? "true" : "false", ctx.count, ctx.errors);
    if (results.data) sb_append(&out, results.data, results.len);
    sb_puts(&out, "]}");
    send_response(fd, status, out.data, out.len);
    sb_free(&results);
    sb_free(&out);
    return status;
}

static int handle_stats(int fd) {
    TPStats s;
    tp_stats(g_pool, &s);
    SBuf out;
    sb_init(&out);
    sb_printf(&out,
              "{\"ok\":true,\"uptime_sec\":%.1f,\"threads\":%d,\"queue_capacity\":%d,\"queued\":%d,"
              "\"active\":%d,\"completed\":%llu,\"rejected\":%llu,\"tables\":[",
              (now_ms() - g_started_at) / 1000.0, s.threads, s.queue_capacity, s.queued, s.active,
              s.completed, s.rejected);
    for (int i = 0; i < db_table_count(g_db); i++) {
        Table *t = db_table_at(g_db, i);
        pthread_rwlock_rdlock(&t->lock);
        sb_printf(&out, "%s{\"name\":\"%s\",\"rows\":%lld,\"index_height\":%d,\"bytes\":%lld}",
                  i ? "," : "", t->name, t->nrows, t->index->height, t->file_size);
        pthread_rwlock_unlock(&t->lock);
    }
    sb_puts(&out, "]}");
    send_response(fd, 200, out.data, out.len);
    sb_free(&out);
    return 200;
}

/* 워커 스레드에서 실행되는 작업 하나 = 연결 하나 */
static void handle_connection(void *arg) {
    Conn *c = arg;
    int fd = c->fd;
    double queued_ms = now_ms() - c->accepted_at;
    double t0 = now_ms();
    free(c);

    Request req;
    memset(&req, 0, sizeof req);
    int status = read_request(fd, &req);
    if (status < 0) {
        free(req.body);
        close(fd);
        return;
    }
    if (status > 0) {
        /* 요청을 끝까지 못 읽은 상태 (413, 431, 400 ...) */
        send_error(fd, status, reason_phrase(status));
        fprintf(stderr, "[worker %d] bad request -> %d\n", tp_worker_id(), status);
        free(req.body);
        close_after_error(fd);
        return;
    } else if (strcmp(req.path, "/query") == 0) {
        status = handle_query(fd, &req);
    } else if (strcmp(req.path, "/health") == 0) {
        status = 200;
        const char *body = "{\"ok\":true}";
        send_response(fd, status, body, strlen(body));
    } else if (strcmp(req.path, "/stats") == 0) {
        status = handle_stats(fd);
    } else {
        status = 404;
        send_error(fd, status, "not found (use POST /query, GET /health, GET /stats)");
    }

    fprintf(stderr, "[worker %d] %s %s -> %d (%.2f ms, waited %.2f ms in queue)\n",
            tp_worker_id(), req.method[0] ? req.method : "-", req.path[0] ? req.path : "-",
            status, now_ms() - t0, queued_ms);
    free(req.body);
    close(fd);
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-d DATA_DIR] [-p PORT] [-t THREADS] [-q QUEUE_CAPACITY]\n", prog);
}

int main(int argc, char **argv) {
    const char *dir = "data";
    int port = 8080, threads = 8, queue_cap = 64;
    int opt;
    while ((opt = getopt(argc, argv, "d:p:t:q:h")) != -1) {
        switch (opt) {
        case 'd': dir = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 'q': queue_cap = atoi(optarg); break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    /* Ctrl+C / SIGTERM → 새 연결 받기를 멈추고, 큐에 남은 요청을 다 처리한 뒤 종료 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* SA_RESTART 없음 → accept() 가 EINTR 로 깨어난다 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);  /* 끊긴 소켓에 써도 프로세스가 죽지 않게 */

    char err[ERR_MAX];
    g_db = db_open(dir, err, sizeof err);
    if (!g_db) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lfd, 128) != 0) {
        perror("listen");
        db_close(g_db);
        return 1;
    }

    g_pool = tp_create(threads, queue_cap);
    g_started_at = now_ms();
    fprintf(stderr, "mini-dbms server listening on :%d (data=%s, %d workers, queue %d, %d tables)\n",
            port, dir, threads, queue_cap, db_table_count(g_db));

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        /* 느린 클라이언트가 워커를 무한정 붙잡지 못하게 타임아웃 */
        struct timeval tv = {SOCKET_TIMEOUT_SEC, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        Conn *c = xmalloc(sizeof *c);
        c->fd = cfd;
        c->accepted_at = now_ms();
        if (tp_submit(g_pool, handle_connection, c) != 0) {
            /* 큐가 가득 참 → 기다리게 하지 않고 바로 거절 */
            send_error(cfd, 503, "server busy, try again later");
            close_after_error(cfd);
            free(c);
        }
    }

    fprintf(stderr, "shutting down: finishing queued requests...\n");
    close(lfd);
    tp_shutdown(g_pool);
    db_close(g_db);
    fprintf(stderr, "bye\n");
    return 0;
}
