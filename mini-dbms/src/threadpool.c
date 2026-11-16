#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

#include "util.h"

typedef struct {
    tp_task_fn fn;
    void *arg;
} Task;

struct ThreadPool {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    Task *queue;          /* 원형 버퍼 */
    int cap, head, count;
    pthread_t *threads;
    int nthreads;
    int shutdown;
    int active;
    unsigned long long completed, rejected;
};

static _Thread_local int g_worker_id = -1;

typedef struct {
    ThreadPool *tp;
    int id;
} WorkerArg;

static void *worker_main(void *p) {
    WorkerArg *wa = p;
    ThreadPool *tp = wa->tp;
    g_worker_id = wa->id;
    free(wa);

    for (;;) {
        pthread_mutex_lock(&tp->mu);
        /* if 가 아니라 while: 깨어났는데 다른 워커가 먼저 작업을 가져갔거나
         * (spurious wakeup) 아직 조건이 안 맞을 수 있으므로 다시 확인한다 */
        while (tp->count == 0 && !tp->shutdown) pthread_cond_wait(&tp->not_empty, &tp->mu);
        if (tp->count == 0 && tp->shutdown) {
            pthread_mutex_unlock(&tp->mu);
            break;
        }
        Task task = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        tp->active++;
        pthread_mutex_unlock(&tp->mu);

        task.fn(task.arg);  /* 실제 작업은 락 밖에서 실행 → 워커들이 병렬로 일한다 */

        pthread_mutex_lock(&tp->mu);
        tp->active--;
        tp->completed++;
        pthread_mutex_unlock(&tp->mu);
    }
    return NULL;
}

ThreadPool *tp_create(int nthreads, int queue_capacity) {
    if (nthreads < 1) nthreads = 1;
    if (queue_capacity < 1) queue_capacity = 1;
    ThreadPool *tp = xcalloc(1, sizeof *tp);
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    tp->cap = queue_capacity;
    tp->queue = xcalloc((size_t)queue_capacity, sizeof(Task));
    tp->nthreads = nthreads;
    tp->threads = xcalloc((size_t)nthreads, sizeof(pthread_t));
    for (int i = 0; i < nthreads; i++) {
        WorkerArg *wa = xmalloc(sizeof *wa);
        wa->tp = tp;
        wa->id = i;
        pthread_create(&tp->threads[i], NULL, worker_main, wa);
    }
    return tp;
}

int tp_submit(ThreadPool *tp, tp_task_fn fn, void *arg) {
    pthread_mutex_lock(&tp->mu);
    if (tp->shutdown || tp->count == tp->cap) {
        tp->rejected++;
        pthread_mutex_unlock(&tp->mu);
        return -1;
    }
    tp->queue[(tp->head + tp->count) % tp->cap] = (Task){fn, arg};
    tp->count++;
    pthread_cond_signal(&tp->not_empty);  /* 잠든 워커 하나를 깨운다 */
    pthread_mutex_unlock(&tp->mu);
    return 0;
}

void tp_shutdown(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);  /* 모든 워커를 깨워 종료를 알린다 */
    pthread_mutex_unlock(&tp->mu);
    for (int i = 0; i < tp->nthreads; i++) pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    free(tp->threads);
    free(tp->queue);
    free(tp);
}

void tp_stats(ThreadPool *tp, TPStats *out) {
    pthread_mutex_lock(&tp->mu);
    out->threads = tp->nthreads;
    out->queue_capacity = tp->cap;
    out->queued = tp->count;
    out->active = tp->active;
    out->completed = tp->completed;
    out->rejected = tp->rejected;
    pthread_mutex_unlock(&tp->mu);
}

int tp_worker_id(void) { return g_worker_id; }
