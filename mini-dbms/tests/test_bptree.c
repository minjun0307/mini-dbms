#include <stdlib.h>

#include "bptree.h"
#include "test.h"

static void shuffle(long long *a, int n, unsigned seed) {
    for (int i = n - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)(seed % (unsigned)(i + 1));
        long long t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int tree_ok(const BPTree *t) {
    char err[256];
    if (bpt_check(t, err, sizeof err)) return 1;
    fprintf(stderr, "    bpt_check: %s\n", err);
    return 0;
}

static void test_empty_tree(void) {
    BPTree *t = bpt_create(4);
    bpval_t v;
    BPIter it;
    CHECK_INT(bpt_search(t, 1, &v), 0);
    bpt_first(t, &it);
    CHECK(!bpt_iter_valid(&it));
    bpt_seek(t, 5, &it);
    CHECK(!bpt_iter_valid(&it));
    CHECK(tree_ok(t));
    bpt_destroy(t);
}

static void test_sequential_insert(void) {
    const int n = 100000;
    BPTree *t = bpt_create(64);
    for (int i = 1; i <= n; i++) CHECK_INT(bpt_insert(t, i, (long long)i * 10), 0);
    CHECK_INT((long long)t->size, n);
    CHECK(tree_ok(t));
    CHECK(t->height <= 4);
    int found = 0;
    for (int i = 1; i <= n; i++) {
        bpval_t v;
        if (bpt_search(t, i, &v) && v == (long long)i * 10) found++;
    }
    CHECK_INT(found, n);
    bpval_t v;
    CHECK_INT(bpt_search(t, 0, &v), 0);
    CHECK_INT(bpt_search(t, n + 1, &v), 0);
    bpt_destroy(t);
}

static void test_random_insert_small_order(void) {
    /* order 가 작으면 분할이 아주 자주 일어나서 split 로직을 철저히 검증할 수 있다 */
    const int n = 20000;
    long long *keys = malloc(sizeof(long long) * n);
    for (int i = 0; i < n; i++) keys[i] = (long long)i * 3 - 30000;  /* 음수 포함 */
    shuffle(keys, n, 42);
    for (int order = 3; order <= 5; order++) {
        BPTree *t = bpt_create(order);
        for (int i = 0; i < n; i++) bpt_insert(t, keys[i], keys[i] + 1);
        CHECK(tree_ok(t));
        int found = 0;
        for (int i = 0; i < n; i++) {
            bpval_t v;
            if (bpt_search(t, keys[i], &v) && v == keys[i] + 1) found++;
        }
        CHECK_INT(found, n);
        bpval_t v;
        CHECK_INT(bpt_search(t, 1, &v), 0);  /* 3의 배수 - 30000 이 아닌 키 */
        bpt_destroy(t);
    }
    free(keys);
}

static void test_invariants_after_every_insert(void) {
    BPTree *t = bpt_create(3);
    int ok = 1;
    for (int i = 0; i < 500 && ok; i++) {
        bpt_insert(t, (i * 7919) % 500, i);
        ok = tree_ok(t);
    }
    CHECK(ok);
    bpt_destroy(t);
}

static void test_duplicate_rejected(void) {
    BPTree *t = bpt_create(4);
    CHECK_INT(bpt_insert(t, 7, 100), 0);
    CHECK_INT(bpt_insert(t, 7, 200), 1);
    bpval_t v = 0;
    CHECK_INT(bpt_search(t, 7, &v), 1);
    CHECK_INT(v, 100);  /* 기존 값이 유지된다 */
    CHECK_INT((long long)t->size, 1);
    bpt_destroy(t);
}

static void test_range_iteration(void) {
    BPTree *t = bpt_create(4);
    for (int i = 0; i < 1000; i++) bpt_insert(t, (long long)i * 2, i);  /* 0, 2, 4, ... 1998 */
    BPIter it;

    bpt_seek(t, 501, &it);  /* 사이 값 → 다음 키부터 */
    CHECK(bpt_iter_valid(&it));
    CHECK_INT(bpt_iter_key(&it), 502);

    int count = 0;
    long long prev = -1;
    int sorted = 1;
    for (bpt_seek(t, 1000, &it); bpt_iter_valid(&it); bpt_iter_next(&it)) {
        if (bpt_iter_key(&it) <= prev) sorted = 0;
        prev = bpt_iter_key(&it);
        count++;
    }
    CHECK(sorted);
    CHECK_INT(count, 500);  /* 1000..1998 */

    bpt_seek(t, 5000, &it);
    CHECK(!bpt_iter_valid(&it));
    bpt_first(t, &it);
    CHECK_INT(bpt_iter_key(&it), 0);
    bpt_destroy(t);
}

void bptree_tests(void) {
    RUN_TEST(test_empty_tree);
    RUN_TEST(test_sequential_insert);
    RUN_TEST(test_random_insert_small_order);
    RUN_TEST(test_invariants_after_every_insert);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_range_iteration);
}
