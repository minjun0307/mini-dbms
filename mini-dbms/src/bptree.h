/*
 * bptree.h — [Phase 2] 메모리 기반 B+ 트리 인덱스
 *
 * key   = 레코드 id
 * value = 데이터 파일 안에서 그 레코드가 시작되는 바이트 위치(offset)
 *
 *   - 내부 노드: 길 안내용 키만 가진다 (어느 자식으로 내려갈지)
 *   - 리프 노드: 실제 (key, value) 를 가진다
 *   - 리프끼리는 next 포인터로 연결 → 범위 검색(id >= 100)을 옆으로 쭉 훑어서 처리
 *
 * order = 노드 하나에 들어갈 수 있는 최대 키 개수. 넘치면 반으로 나눈다(split).
 */
#ifndef BPTREE_H
#define BPTREE_H

#include <stddef.h>

typedef long long bpkey_t;
typedef long long bpval_t;

typedef struct BPNode BPNode;

typedef struct {
    BPNode *root;
    int order;
    size_t size;    /* 저장된 키 개수 */
    int height;     /* 루트~리프 높이 (리프만 있으면 1) */
    size_t nodes;   /* 노드 개수 */
} BPTree;

typedef struct {
    const BPNode *leaf;
    int idx;
} BPIter;

BPTree *bpt_create(int order);
void bpt_destroy(BPTree *t);

/* 0: 삽입 성공, 1: 이미 있는 키 (아무것도 바꾸지 않음) */
int bpt_insert(BPTree *t, bpkey_t key, bpval_t val);

/* 1: 찾음 (*val 에 값), 0: 없음 */
int bpt_search(const BPTree *t, bpkey_t key, bpval_t *val);

/* 범위 순회 */
void bpt_seek(const BPTree *t, bpkey_t key, BPIter *it);  /* key 이상인 첫 위치 */
void bpt_first(const BPTree *t, BPIter *it);              /* 가장 작은 키 */
int bpt_iter_valid(const BPIter *it);
bpkey_t bpt_iter_key(const BPIter *it);
bpval_t bpt_iter_val(const BPIter *it);
void bpt_iter_next(BPIter *it);

/* 테스트용: 모든 불변식(정렬, 키 범위, 리프 깊이 동일, 채움 비율, 리프 연결)을 검사. 1 = 정상 */
int bpt_check(const BPTree *t, char *err, size_t errlen);

#endif
