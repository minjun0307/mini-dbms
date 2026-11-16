#include "table.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BPT_ORDER 64   /* 노드 하나에 키 최대 64개 → 100만 건이어도 높이 4 정도 */

const char *coltype_name(ColType t) { return t == COL_INT ? "INT" : "TEXT"; }

int schema_find(const Schema *s, const char *name) {
    for (int i = 0; i < s->ncols; i++)
        if (str_ieq(s->cols[i].name, name)) return i;
    return -1;
}

int schema_load(const char *path, Schema *out, char *err, size_t errlen) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errlen, "cannot open schema %s: %s", path, strerror(errno));
        return -1;
    }
    memset(out, 0, sizeof *out);
    char line[256];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char name[NAME_MAX_LEN + 1], type[32], extra[2];
        int n = sscanf(line, "%64s %31s %1s", name, type, extra);
        if (n <= 0) continue;  /* 빈 줄 / 주석 */
        if (n != 2) {
            snprintf(err, errlen, "%s line %d: expected '<column> <INT|TEXT>'", path, lineno);
            rc = -1;
            break;
        }
        if (!is_valid_name(name)) {
            snprintf(err, errlen, "%s line %d: invalid column name '%s'", path, lineno, name);
            rc = -1;
            break;
        }
        if (out->ncols >= MAX_COLUMNS) {
            snprintf(err, errlen, "%s: too many columns (max %d)", path, MAX_COLUMNS);
            rc = -1;
            break;
        }
        if (schema_find(out, name) >= 0) {
            snprintf(err, errlen, "%s line %d: duplicate column '%s'", path, lineno, name);
            rc = -1;
            break;
        }
        ColType ct;
        if (str_ieq(type, "INT")) ct = COL_INT;
        else if (str_ieq(type, "TEXT")) ct = COL_TEXT;
        else {
            snprintf(err, errlen, "%s line %d: unknown type '%s' (use INT or TEXT)", path, lineno, type);
            rc = -1;
            break;
        }
        strcpy(out->cols[out->ncols].name, name);
        out->cols[out->ncols].type = ct;
        out->ncols++;
    }
    fclose(fp);
    if (rc != 0) return rc;
    if (out->ncols < 2 || strcmp(out->cols[0].name, "id") != 0 || out->cols[0].type != COL_INT) {
        snprintf(err, errlen, "%s: first column must be 'id INT' (auto-assigned), followed by at least one column", path);
        return -1;
    }
    return 0;
}

/* ---------- 행 직렬화: Value[] ↔ CSV 한 줄 ---------- */

void row_encode(const Schema *s, const Value *row, SBuf *out) {
    for (int c = 0; c < s->ncols; c++) {
        if (c) sb_putc(out, ',');
        if (s->cols[c].type == COL_INT) {
            char num[24];
            int n = snprintf(num, sizeof num, "%lld", row[c].i);
            sb_append(out, num, (size_t)n);
        } else {
            sb_putc(out, '"');
            for (const char *p = row[c].s; *p; p++) {
                if (*p == '"') sb_putc(out, '"');  /* " → "" */
                sb_putc(out, *p);
            }
            sb_putc(out, '"');
        }
    }
    sb_putc(out, '\n');
}

void row_free(const Schema *s, Value *row) {
    for (int c = 0; c < s->ncols; c++) value_free(&row[c]);
}

int row_decode(const Schema *s, const char *line, size_t len, Value *row) {
    if (len && line[len - 1] == '\n') len--;
    for (int c = 0; c < s->ncols; c++) {
        row[c].type = s->cols[c].type == COL_INT ? VAL_INT : VAL_TEXT;
        row[c].i = 0;
        row[c].s = NULL;
    }
    size_t i = 0;
    for (int c = 0; c < s->ncols; c++) {
        if (c > 0) {
            if (i >= len || line[i] != ',') goto bad;
            i++;
        }
        if (s->cols[c].type == COL_INT) {
            char *end;
            errno = 0;
            long long v = strtoll(line + i, &end, 10);
            size_t used = (size_t)(end - (line + i));
            if (used == 0 || errno != 0 || i + used > len) goto bad;
            row[c].i = v;
            i += used;
        } else {
            if (i >= len || line[i] != '"') goto bad;
            i++;
            char *buf = xmalloc(len - i + 1);
            size_t n = 0;
            for (;;) {
                if (i >= len) {
                    free(buf);
                    goto bad;
                }
                char ch = line[i++];
                if (ch == '"') {
                    if (i < len && line[i] == '"') {  /* "" → " */
                        buf[n++] = '"';
                        i++;
                    } else {
                        break;  /* 닫는 따옴표 */
                    }
                } else {
                    buf[n++] = ch;
                }
            }
            buf[n] = '\0';
            row[c].s = buf;
        }
    }
    if (i != len) goto bad;
    return 0;
bad:
    row_free(s, row);
    return -1;
}

/* ---------- 열기 + 크래시 복구 + 인덱스 재구축 ---------- */

static int index_add(Table *t, long long id, long long off, char *err, size_t errlen) {
    if (bpt_insert(t->index, id, off) != 0) {
        snprintf(err, errlen, "corrupted data in %s: duplicate id %lld at byte offset %lld",
                 t->data_path, id, off);
        return -1;
    }
    t->nrows++;
    if (id >= t->next_id) t->next_id = id + 1;
    return 0;
}

static int load_and_recover(Table *t, char *err, size_t errlen) {
    t->next_id = 1;
    FILE *fp = fopen(t->data_path, "rb");
    if (!fp) {
        if (errno == ENOENT) return 0;  /* 아직 데이터가 없는 빈 테이블 */
        snprintf(err, errlen, "cannot open %s: %s", t->data_path, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long long pos = 0, truncate_at = -1;
    long long batch_start = -1, batch_left = 0;
    long long *pending = NULL;   /* 묶음 안의 (id, offset) 쌍: 묶음이 완성돼야 인덱스에 넣는다 */
    size_t npending = 0, pending_cap = 0;
    int rc = 0;

    while ((len = getline(&line, &cap, fp)) > 0) {
        long long off = pos;
        pos += len;

        if (line[len - 1] != '\n') {  /* 마지막 줄이 끝나지 않음 = 쓰는 도중 중단 */
            truncate_at = batch_left > 0 ? batch_start : off;
            break;
        }
        if (line[0] == '@') {         /* 묶음 시작 표시 "@B n" */
            long long n = 0;
            if (batch_left > 0 || sscanf(line, "@B %lld", &n) != 1 || n <= 0) {
                snprintf(err, errlen, "corrupted batch marker in %s at byte offset %lld", t->data_path, off);
                rc = -1;
                break;
            }
            batch_start = off;
            batch_left = n;
            npending = 0;
            continue;
        }

        /* 시작할 때는 id 만 읽으면 된다 (행 전체 파싱은 SELECT 할 때) */
        char *end;
        errno = 0;
        long long id = strtoll(line, &end, 10);
        if (end == line || errno != 0 || (*end != ',' && *end != '\n')) {
            snprintf(err, errlen, "corrupted row in %s at byte offset %lld", t->data_path, off);
            rc = -1;
            break;
        }

        if (batch_left > 0) {
            if (npending + 2 > pending_cap) {
                pending_cap = pending_cap ? pending_cap * 2 : 64;
                pending = xrealloc(pending, sizeof(long long) * pending_cap);
            }
            pending[npending++] = id;
            pending[npending++] = off;
            if (--batch_left == 0) {  /* 묶음 완성 → 이제서야 인덱스에 반영 */
                for (size_t i = 0; i < npending && rc == 0; i += 2)
                    rc = index_add(t, pending[i], pending[i + 1], err, errlen);
                npending = 0;
                if (rc != 0) break;
            }
        } else if (index_add(t, id, off, err, errlen) != 0) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && truncate_at < 0 && batch_left > 0)
        truncate_at = batch_start;  /* 파일이 끝났는데 묶음이 덜 옴 */

    free(line);
    free(pending);
    fclose(fp);
    if (rc != 0) return rc;

    if (truncate_at >= 0) {
        if (truncate(t->data_path, (off_t)truncate_at) != 0) {
            snprintf(err, errlen, "cannot truncate %s: %s", t->data_path, strerror(errno));
            return -1;
        }
        t->recovered_bytes = pos - truncate_at;
        t->file_size = truncate_at;
    } else {
        t->file_size = pos;
    }
    return 0;
}

Table *table_open(const char *dir, const char *name, char *err, size_t errlen) {
    if (!is_valid_name(name)) {  /* "../etc/passwd" 같은 경로 공격 차단 */
        snprintf(err, errlen, "invalid table name '%s'", name);
        return NULL;
    }
    Table *t = xcalloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    snprintf(t->schema_path, sizeof t->schema_path, "%s/%s.schema", dir, name);
    snprintf(t->data_path, sizeof t->data_path, "%s/%s.tbl", dir, name);

    if (schema_load(t->schema_path, &t->schema, err, errlen) != 0) {
        free(t);
        return NULL;
    }
    t->index = bpt_create(BPT_ORDER);
    if (load_and_recover(t, err, errlen) != 0) {
        bpt_destroy(t->index);
        free(t);
        return NULL;
    }
    t->wfp = fopen(t->data_path, "ab");
    if (!t->wfp) {
        snprintf(err, errlen, "cannot open %s for writing: %s", t->data_path, strerror(errno));
        bpt_destroy(t->index);
        free(t);
        return NULL;
    }
    /* 쓰기 우선 rwlock: glibc 기본값은 "읽기 우선"이라, SELECT 가 끊임없이 들어오면
     * 읽기 락이 한 번도 0 이 되지 않아 INSERT 가 영원히 못 들어간다 (writer starvation).
     * 쓰기 우선으로 바꾸면 쓰기가 대기 중일 때 새 읽기는 그 뒤에 줄을 선다. */
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&t->lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    return t;
}

void table_close(Table *t) {
    if (!t) return;
    if (t->wfp) fclose(t->wfp);
    bpt_destroy(t->index);
    pthread_rwlock_destroy(&t->lock);
    free(t);
}

/* ---------- 쓰기 ---------- */

int table_append(Table *t, Value *rows, int nrows, long long *first_id, char *err, size_t errlen) {
    int nc = t->schema.ncols;
    SBuf buf;
    sb_init(&buf);
    long long *offsets = xmalloc(sizeof(long long) * (size_t)nrows);

    if (nrows > 1) sb_printf(&buf, "@B %d\n", nrows);
    for (int r = 0; r < nrows; r++) {
        rows[(size_t)r * nc].type = VAL_INT;
        rows[(size_t)r * nc].i = t->next_id + r;     /* id 자동 부여 */
        offsets[r] = t->file_size + (long long)buf.len;
        row_encode(&t->schema, &rows[(size_t)r * nc], &buf);
    }

    /* 묶음 전체를 한 번에 기록하고 OS 로 내려보낸다 */
    size_t written = fwrite(buf.data, 1, buf.len, t->wfp);
    if (written != buf.len || fflush(t->wfp) != 0) {
        snprintf(err, errlen, "write to %s failed: %s", t->data_path, strerror(errno));
        /* 되돌리기: 일부만 쓰였을 수 있으니 이전 끝 위치로 잘라내고 핸들을 다시 연다 */
        fclose(t->wfp);
        if (truncate(t->data_path, (off_t)t->file_size) != 0) { /* 다음 open 때 복구됨 */ }
        t->wfp = fopen(t->data_path, "ab");
        sb_free(&buf);
        free(offsets);
        return -1;
    }

    /* 파일에 다 쓰인 뒤에만 인덱스와 메타데이터를 갱신한다 */
    for (int r = 0; r < nrows; r++)
        bpt_insert(t->index, rows[(size_t)r * nc].i, offsets[r]);
    *first_id = t->next_id;
    t->next_id += nrows;
    t->nrows += nrows;
    t->file_size += (long long)buf.len;

    sb_free(&buf);
    free(offsets);
    return 0;
}

/* ---------- 읽기 ---------- */

int reader_open(Table *t, TableReader *r, char *err, size_t errlen) {
    memset(r, 0, sizeof *r);
    r->end = t->file_size;
    r->fp = fopen(t->data_path, "rb");
    if (!r->fp) {
        snprintf(err, errlen, "cannot open %s: %s", t->data_path, strerror(errno));
        return -1;
    }
    return 0;
}

void reader_close(TableReader *r) {
    if (r->fp) fclose(r->fp);
    free(r->buf);
    memset(r, 0, sizeof *r);
}

int reader_next(Table *t, TableReader *r, Value *row, long long *offset, char *err, size_t errlen) {
    while (r->pos < r->end) {
        ssize_t len = getline(&r->buf, &r->cap, r->fp);
        if (len <= 0) return 0;
        long long off = r->pos;
        r->pos += len;
        if (r->buf[0] == '@') continue;  /* 묶음 표시 줄은 데이터가 아님 */
        if (row_decode(&t->schema, r->buf, (size_t)len, row) != 0) {
            snprintf(err, errlen, "corrupted row in %s at byte offset %lld", t->data_path, off);
            return -1;
        }
        if (offset) *offset = off;
        return 1;
    }
    return 0;
}

int reader_read_at(Table *t, TableReader *r, long long offset, Value *row, char *err, size_t errlen) {
    if (fseeko(r->fp, (off_t)offset, SEEK_SET) != 0) {
        snprintf(err, errlen, "seek to %lld in %s failed", offset, t->data_path);
        return -1;
    }
    ssize_t len = getline(&r->buf, &r->cap, r->fp);
    if (len <= 0 || row_decode(&t->schema, r->buf, (size_t)len, row) != 0) {
        snprintf(err, errlen, "cannot read row at byte offset %lld in %s", offset, t->data_path);
        return -1;
    }
    r->pos = offset + len;
    return 0;
}
