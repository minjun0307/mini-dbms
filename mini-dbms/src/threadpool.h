/*
 * threadpool.h — [Phase 3] 고정 크기 스레드 풀
 *
 *   accept 스레드 ──submit──▶ [ 작업 큐 (원형 버퍼, 크기 제한) ] ──▶ 워커 N 개가 꺼내서 실행
 *
 * - 요청마다 스레드를 새로 만들지 않고, 미리 만든 N 개의 스레드를 재사용한다.
 * - 큐가 가득 차면 submit 이 즉시 실패한다 → 서버는 503 을 돌려준다 (backpressure).
 *   무한정 쌓아 두면 메모리가 늘고 응답 지연이 끝없이 길어지기 때문.
 * - 동기화: mutex 1개 + 조건 변수 1개 (큐가 비면 워커는 잠든다)
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef void (*tp_task_fn)(void *arg);
typedef struct ThreadPool ThreadPool;

typedef struct {
    int threads;
    int queue_capacity;
    int queued;
    int active;
    unsigned long long completed;
    unsigned long long rejected;
} TPStats;

ThreadPool *tp_create(int nthreads, int queue_capacity);
int tp_submit(ThreadPool *tp, tp_task_fn fn, void *arg);  /* 0 = 접수, -1 = 큐가 가득 참/종료 중 */
void tp_shutdown(ThreadPool *tp);                          /* 남은 작업을 끝내고 스레드를 정리 */
void tp_stats(ThreadPool *tp, TPStats *out);
int tp_worker_id(void);                                    /* 현재 스레드의 워커 번호, 워커가 아니면 -1 */

#endif
