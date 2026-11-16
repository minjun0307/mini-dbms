#include "bptree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

struct BPNode {
    int is_leaf;
    int nkeys;
    bpkey_t *keys;       /* 용량 order+1: 분할 직전 한 칸 넘칠 수 있게 여유를 둔다 */
    BPNode **children;   /* 내부 노드: 용량 order+2 (자식 수 = 키 수 + 1) */
    bpval_t *vals;       /* 리프 노드: 용량 order+1 */
    BPNode *next;        /* 리프 노드: 오른쪽 리프 (범위 검색용) */
};

static BPNode *node_new(BPTree *t, int is_leaf) {
    BPNode *n = xcalloc(1, sizeof *n);
    n->is_leaf = is_leaf;
    n->keys = xmalloc(sizeof(bpkey_t) * (size_t)(t->order + 1));
    if (is_leaf)
        n->vals = xmalloc(sizeof(bpval_t) * (size_t)(t->order + 1));
    else
        n->children = xmalloc(sizeof(BPNode *) * (size_t)(t->order + 2));
    t->nodes++;
    return n;
}

static void node_free(BPNode *n) {
    if (!n) return;
    if (!n->is_leaf)
        for (int i = 0; i <= n->nkeys; i++) node_free(n->children[i]);
    free(n->keys);
    free(n->vals);
    free(n->children);
    free(n);
}

BPTree *bpt_create(int order) {
    if (order < 3) order = 3;
    BPTree *t = xcalloc(1, sizeof *t);
    t->order = order;
    t->root = node_new(t, 1);  /* 처음엔 빈 리프 하나가 루트 */
    t->height = 1;
    return t;
}

void bpt_destroy(BPTree *t) {
    if (!t) return;
    node_free(t->root);
    free(t);
}

/* keys[0..n) 에서 key "이상"인 첫 위치 (이진 탐색) */
static int lower_bound(const bpkey_t *keys, int n, bpkey_t key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* keys[0..n) 에서 key "초과"인 첫 위치 = 내부 노드에서 내려갈 자식 번호.
 * 내부 노드의 keys[i] 는 "children[i+1] 서브트리의 최솟값" 이므로
 * key 와 같은 구분 키를 만나면 오른쪽으로 가야 한다. */
static int upper_bound(const bpkey_t *keys, int n, bpkey_t key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid] <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* 루트에서 시작해 key 가 있어야 할 리프까지 내려간다: 높이만큼만 비교 → O(log n) */
static const BPNode *find_leaf(const BPTree *t, bpkey_t key) {
    const BPNode *n = t->root;
    while (!n->is_leaf) n = n->children[upper_bound(n->keys, n->nkeys, key)];
    return n;
}

int bpt_search(const BPTree *t, bpkey_t key, bpval_t *val) {
    const BPNode *leaf = find_leaf(t, key);
    int i = lower_bound(leaf->keys, leaf->nkeys, key);
    if (i < leaf->nkeys && leaf->keys[i] == key) {
        if (val) *val = leaf->vals[i];
        return 1;
    }
    return 0;
}

/*
 * 재귀 삽입.
 * 반환값: 0 = 정상, -1 = 중복 키, 1 = 이 노드가 분할됨
 *         (분할되면 부모가 넣어야 할 구분 키 *up_key 와 새 오른쪽 노드 *up_node 를 돌려준다)
 */
static int insert_rec(BPTree *t, BPNode *n, bpkey_t key, bpval_t val,
                      bpkey_t *up_key, BPNode **up_node) {
    if (n->is_leaf) {
        int pos = lower_bound(n->keys, n->nkeys, key);
        if (pos < n->nkeys && n->keys[pos] == key) return -1;

        /* 정렬 순서를 유지하도록 한 칸씩 밀고 끼워 넣는다 */
        memmove(&n->keys[pos + 1], &n->keys[pos], sizeof(bpkey_t) * (size_t)(n->nkeys - pos));
        memmove(&n->vals[pos + 1], &n->vals[pos], sizeof(bpval_t) * (size_t)(n->nkeys - pos));
        n->keys[pos] = key;
        n->vals[pos] = val;
        n->nkeys++;
        if (n->nkeys <= t->order) return 0;

        /* 리프 분할: 오른쪽 절반을 새 리프로 옮기고, 오른쪽 첫 키를 "복사"해서 부모에 올린다.
         * (B+ 트리에서는 실제 데이터가 항상 리프에 남아 있어야 하므로 복사) */
        int mid = n->nkeys / 2;
        BPNode *r = node_new(t, 1);
        r->nkeys = n->nkeys - mid;
        memcpy(r->keys, &n->keys[mid], sizeof(bpkey_t) * (size_t)r->nkeys);
        memcpy(r->vals, &n->vals[mid], sizeof(bpval_t) * (size_t)r->nkeys);
        n->nkeys = mid;
        r->next = n->next;  /* 리프 연결 리스트에 끼워 넣기 */
        n->next = r;
        *up_key = r->keys[0];
        *up_node = r;
        return 1;
    }

    int ci = upper_bound(n->keys, n->nkeys, key);
    bpkey_t child_key;
    BPNode *child_new;
    int rc = insert_rec(t, n->children[ci], key, val, &child_key, &child_new);
    if (rc != 1) return rc;

    /* 자식이 분할됨 → 구분 키와 새 자식을 ci 자리에 끼워 넣는다 */
    memmove(&n->keys[ci + 1], &n->keys[ci], sizeof(bpkey_t) * (size_t)(n->nkeys - ci));
    memmove(&n->children[ci + 2], &n->children[ci + 1], sizeof(BPNode *) * (size_t)(n->nkeys - ci));
    n->keys[ci] = child_key;
    n->children[ci + 1] = child_new;
    n->nkeys++;
    if (n->nkeys <= t->order) return 0;

    /* 내부 노드 분할: 가운데 키는 부모로 "이동"한다 (내부 노드엔 데이터가 없으므로 복사 불필요) */
    int mid = n->nkeys / 2;
    BPNode *r = node_new(t, 0);
    r->nkeys = n->nkeys - mid - 1;
    memcpy(r->keys, &n->keys[mid + 1], sizeof(bpkey_t) * (size_t)r->nkeys);
    memcpy(r->children, &n->children[mid + 1], sizeof(BPNode *) * (size_t)(r->nkeys + 1));
    *up_key = n->keys[mid];
    *up_node = r;
    n->nkeys = mid;
    return 1;
}

int bpt_insert(BPTree *t, bpkey_t key, bpval_t val) {
    bpkey_t up_key;
    BPNode *up_node;
    int rc = insert_rec(t, t->root, key, val, &up_key, &up_node);
    if (rc < 0) return 1;
    if (rc == 1) {
        /* 루트가 분할되면 새 루트를 만든다 → 트리 높이가 1 늘어나는 유일한 경우 */
        BPNode *root = node_new(t, 0);
        root->nkeys = 1;
        root->keys[0] = up_key;
        root->children[0] = t->root;
        root->children[1] = up_node;
        t->root = root;
        t->height++;
    }
    t->size++;
    return 0;
}

/* ---------- 범위 순회 ---------- */

static void iter_normalize(BPIter *it) {
    while (it->leaf && it->idx >= it->leaf->nkeys) {
        it->leaf = it->leaf->next;
        it->idx = 0;
    }
}

void bpt_seek(const BPTree *t, bpkey_t key, BPIter *it) {
    const BPNode *leaf = find_leaf(t, key);
    it->leaf = leaf;
    it->idx = lower_bound(leaf->keys, leaf->nkeys, key);
    iter_normalize(it);  /* 이 리프에 없으면 다음 리프의 첫 키가 답 */
}

void bpt_first(const BPTree *t, BPIter *it) {
    const BPNode *n = t->root;
    while (!n->is_leaf) n = n->children[0];
    it->leaf = n;
    it->idx = 0;
    iter_normalize(it);
}

int bpt_iter_valid(const BPIter *it) { return it->leaf != NULL; }
bpkey_t bpt_iter_key(const BPIter *it) { return it->leaf->keys[it->idx]; }
bpval_t bpt_iter_val(const BPIter *it) { return it->leaf->vals[it->idx]; }

void bpt_iter_next(BPIter *it) {
    it->idx++;
    iter_normalize(it);
}

/* ---------- 불변식 검사 (테스트용) ---------- */

typedef struct {
    int leaf_depth;
    size_t count;
    char *err;
    size_t errlen;
} CheckCtx;

static int check_node(const BPTree *t, const BPNode *n, int depth,
                      int has_lo, bpkey_t lo, int has_hi, bpkey_t hi, CheckCtx *c) {
    if (n->nkeys > t->order) {
        snprintf(c->err, c->errlen, "node overflow: %d keys > order %d", n->nkeys, t->order);
        return 0;
    }
    if (n != t->root) {
        int min = n->is_leaf ? (t->order + 1) / 2 : t->order / 2;
        if (n->nkeys < min) {
            snprintf(c->err, c->errlen, "underfilled %s node: %d keys < %d",
                     n->is_leaf ? "leaf" : "internal", n->nkeys, min);
            return 0;
        }
    }
    for (int i = 0; i < n->nkeys; i++) {
        if (i > 0 && n->keys[i - 1] >= n->keys[i]) {
            snprintf(c->err, c->errlen, "keys not strictly increasing at depth %d", depth);
            return 0;
        }
        if ((has_lo && n->keys[i] < lo) || (has_hi && n->keys[i] >= hi)) {
            snprintf(c->err, c->errlen, "key %lld outside of its separator range", n->keys[i]);
            return 0;
        }
    }
    if (n->is_leaf) {
        if (c->leaf_depth < 0) c->leaf_depth = depth;
        else if (c->leaf_depth != depth) {
            snprintf(c->err, c->errlen, "leaves at different depths (%d vs %d)", c->leaf_depth, depth);
            return 0;
        }
        c->count += (size_t)n->nkeys;
        return 1;
    }
    for (int i = 0; i <= n->nkeys; i++) {
        int clo = i == 0 ? has_lo : 1;
        bpkey_t vlo = i == 0 ? lo : n->keys[i - 1];
        int chi = i == n->nkeys ? has_hi : 1;
        bpkey_t vhi = i == n->nkeys ? hi : n->keys[i];
        if (!check_node(t, n->children[i], depth + 1, clo, vlo, chi, vhi, c)) return 0;
    }
    return 1;
}

int bpt_check(const BPTree *t, char *err, size_t errlen) {
    CheckCtx c = {-1, 0, err, errlen};
    if (!check_node(t, t->root, 1, 0, 0, 0, 0, &c)) return 0;
    if (c.leaf_depth != t->height) {
        snprintf(err, errlen, "height mismatch: leaves at %d, height %d", c.leaf_depth, t->height);
        return 0;
    }
    if (c.count != t->size) {
        snprintf(err, errlen, "size mismatch: counted %zu, size %zu", c.count, t->size);
        return 0;
    }
    /* 리프 연결 리스트로 훑어도 같은 개수가 정렬된 순서로 나와야 한다 */
    BPIter it;
    size_t chained = 0;
    int first = 1;
    bpkey_t prev = 0;
    for (bpt_first(t, &it); bpt_iter_valid(&it); bpt_iter_next(&it)) {
        bpkey_t k = bpt_iter_key(&it);
        if (!first && prev >= k) {
            snprintf(err, errlen, "leaf chain not sorted at key %lld", k);
            return 0;
        }
        prev = k;
        first = 0;
        chained++;
    }
    if (chained != t->size) {
        snprintf(err, errlen, "leaf chain has %zu keys, expected %zu", chained, t->size);
        return 0;
    }
    return 1;
}
