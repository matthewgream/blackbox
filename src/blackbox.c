/*
 * blackbox — core implementation.  See include/blackbox.h and DESIGN.md.
 *
 * The store is a byte buffer (the caller's pool) holding newline-terminated CSV lines back to back.
 *   PERSIST_NONE : the pool IS the store — a ring; inserts evict the oldest line when full.
 *   PERSIST_FILE : the pool STAGES lines; flush() appends them to a file and empties the pool.
 * A `cursor` for pull() is a byte offset (into the pool for NONE, into the file for FILE).
 *
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */
#include "blackbox.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_MDS_FLASH
#error "BLACKBOX_PERSIST_MDS_FLASH backend is not implemented yet (P3) — use PERSIST_NONE (RTC pool) or PERSIST_FILE"
#endif
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_CUSTOM
#error "BLACKBOX_PERSIST_CUSTOM backend is not wired yet"
#endif

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
typedef struct { const char *path; } file_be_t;
#endif

/* -------- helpers ----------------------------------------------------------------------------- */

static uint8_t used_pct(const blackbox_handle_t *h) {
    if (!h->cfg->pool_sz) return 0;
    return (uint8_t)((h->pool_len * 100u) / h->cfg->pool_sz);
}

/* Append one assembled line (ln bytes, includes the trailing '\n') into the store. */
static int store_append(blackbox_handle_t *h, const char *line, size_t ln) {
    if (ln == 0 || ln > h->cfg->pool_sz)
        return -1;

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    if (h->pool_len + ln > h->cfg->pool_sz)     /* stage full → drain to the file first */
        if (blackbox_flush(h) != 0)
            return -1;
    if (h->pool_len + ln > h->cfg->pool_sz)     /* still no room (shouldn't happen) */
        return -1;
    memcpy(h->cfg->pool + h->pool_len, line, ln);
    h->pool_len += ln;
#else /* PERSIST_NONE — the pool is a ring; evict whole oldest lines to make room */
    while (h->pool_len + ln > h->cfg->pool_sz && h->pool_len > 0) {
        const char *nl = memchr(h->cfg->pool, '\n', h->pool_len);
        const size_t evict = nl ? (size_t)(nl - h->cfg->pool) + 1u : h->pool_len;
        memmove(h->cfg->pool, h->cfg->pool + evict, h->pool_len - evict);
        h->pool_len -= evict;
        if (h->count) h->count--;
        h->stored_bytes -= (h->stored_bytes >= (uint32_t)evict) ? (uint32_t)evict : h->stored_bytes;
    }
    if (h->pool_len + ln > h->cfg->pool_sz)
        return -1;
    memcpy(h->cfg->pool + h->pool_len, line, ln);
    h->pool_len += ln;
#endif
    h->count++;
    h->stored_bytes += (uint32_t)ln;
    return 0;
}

/* -------- lifecycle --------------------------------------------------------------------------- */

int blackbox_init(blackbox_handle_t *h, const blackbox_config_t *cfg) {
    if (!h || !cfg || !cfg->pool || cfg->pool_sz < BLACKBOX_LINE_MAX)
        return -1;
    memset(h, 0, sizeof(*h));
    h->cfg = cfg;
    h->enabled = cfg->enabled;

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    file_be_t *be = (file_be_t *)calloc(1, sizeof(*be));
    if (!be) return -1;
    be->path = cfg->persist_arg ? cfg->persist_arg : "blackbox.csv";
    FILE *f = fopen(be->path, "a");            /* ensure it exists / is appendable */
    if (!f) { free(be); return -1; }
    (void)fclose(f);
    h->be = be;
#endif
    return 0;
}

int blackbox_insert(blackbox_handle_t *h, const blackbox_struct_config_t *sc, const void *data) {
    if (!h || !sc || !sc->encode) return -1;
    if (!h->enabled) { h->dropped++; return 0; }

    char payload[BLACKBOX_LINE_MAX];
    const int pn = sc->encode(sc, data, payload, sizeof(payload));
    if (pn < 0 || pn >= (int)sizeof(payload)) { h->dropped++; return -1; }

    char clk[48];
    const int cn = BLACKBOX_CLOCK(clk, sizeof(clk));
    if (cn < 0 || cn >= (int)sizeof(clk)) { h->dropped++; return -1; }

    char line[BLACKBOX_LINE_MAX];
    const int ln = snprintf(line, sizeof(line), "%s,%s,%s\n", sc->tag, clk, payload);
    if (ln <= 0 || ln >= (int)sizeof(line)) { h->dropped++; return -1; }

    if (store_append(h, line, (size_t)ln) != 0) { h->dropped++; return -1; }
    h->inserted++;

    if (h->cfg->flush == BLACKBOX_FLUSH_WRITE_THROUGH)
        return blackbox_flush(h);
    return 0;
}

void blackbox_tick(blackbox_handle_t *h, uint32_t dt_ms) {
    if (!h) return;
    h->uptime_ms += dt_ms;
    if (h->cfg->flush == BLACKBOX_FLUSH_BATCH_TIME && h->cfg->flush_ms) {
        h->flush_timer_ms += dt_ms;
        if (h->flush_timer_ms >= h->cfg->flush_ms) {
            h->flush_timer_ms = 0;
            (void)blackbox_flush(h);
        }
    }
}

int blackbox_flush(blackbox_handle_t *h) {
    if (!h) return -1;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    if (h->pool_len == 0) return 0;
    file_be_t *be = (file_be_t *)h->be;
    FILE *f = fopen(be->path, "a");
    if (!f) return -1;
    const size_t w = fwrite(h->cfg->pool, 1, h->pool_len, f);
    (void)fflush(f);
    (void)fclose(f);
    if (w != h->pool_len) return -1;
    h->pool_len = 0;                            /* staged bytes are now durable in the file */
    h->flushes++;
#else
    (void)h;                                    /* PERSIST_NONE — the pool already IS the store */
#endif
    return 0;
}

int blackbox_pull(blackbox_handle_t *h, size_t *cursor, char *line, size_t n) {
    if (!h || !cursor || !line || n == 0) return -1;

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    if (*cursor == 0) (void)blackbox_flush(h);  /* make staged records visible to the read */
    file_be_t *be = (file_be_t *)h->be;
    FILE *f = fopen(be->path, "r");
    if (!f) return 0;
    if (fseek(f, (long)*cursor, SEEK_SET) != 0) { (void)fclose(f); return 0; }
    char *g = fgets(line, (int)n, f);
    if (!g) { (void)fclose(f); return 0; }
    *cursor = (size_t)ftell(f);
    (void)fclose(f);
#else
    if (*cursor >= h->pool_len) return 0;
    const char *base = h->cfg->pool + *cursor;
    const size_t left = h->pool_len - *cursor;
    const char *nl = memchr(base, '\n', left);
    size_t ll = nl ? (size_t)(nl - base) + 1u : left;
    size_t copy = (ll < n - 1) ? ll : n - 1;
    memcpy(line, base, copy);
    line[copy] = '\0';
    *cursor += ll;
#endif
    /* strip the trailing newline for the caller */
    size_t len = strlen(line);
    if (len && line[len - 1] == '\n') line[--len] = '\0';
    return (int)len;
}

void blackbox_clear(blackbox_handle_t *h) {
    if (!h) return;
    h->pool_len = 0;
    h->count = 0;
    h->stored_bytes = 0;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    file_be_t *be = (file_be_t *)h->be;
    FILE *f = fopen(be->path, "w");             /* truncate */
    if (f) (void)fclose(f);
#endif
}

void blackbox_expire(blackbox_handle_t *h) {
    if (!h || h->cfg->expire_limit == 0) return;
#if BLACKBOX_PERSIST != BLACKBOX_PERSIST_FILE
    /* keep only the newest `expire_limit` records (evict oldest whole lines) */
    while (h->count > h->cfg->expire_limit && h->pool_len > 0) {
        const char *nl = memchr(h->cfg->pool, '\n', h->pool_len);
        const size_t evict = nl ? (size_t)(nl - h->cfg->pool) + 1u : h->pool_len;
        memmove(h->cfg->pool, h->cfg->pool + evict, h->pool_len - evict);
        h->pool_len -= evict;
        if (h->count) h->count--;
        h->stored_bytes -= (h->stored_bytes >= (uint32_t)evict) ? (uint32_t)evict : h->stored_bytes;
    }
#endif
}

bool blackbox_status(blackbox_handle_t *h, blackbox_status_t *out) {
    if (!h || !out) return false;
    out->enabled  = h->enabled;
    out->filters  = 0;                          /* per-record-type filters: not yet */
    out->persist  = BLACKBOX_PERSIST;
    out->count    = h->count;
    out->dropped  = h->dropped;
    out->inserted = h->inserted;
    out->flushes  = h->flushes;
    out->bytes    = h->stored_bytes;
    out->pool_sz  = (uint32_t)h->cfg->pool_sz;
    out->used_pct = used_pct(h);
    out->uptime_ms = h->uptime_ms;
    return true;
}

static const char *persist_name(int p) {
    switch (p) {
    case BLACKBOX_PERSIST_NONE:      return "none";
    case BLACKBOX_PERSIST_FILE:      return "file";
    case BLACKBOX_PERSIST_MDS_FLASH: return "mds-flash";
    case BLACKBOX_PERSIST_CUSTOM:    return "custom";
    default:                         return "?";
    }
}

int blackbox_status_str(const blackbox_status_t *st, uint32_t flags, char *buf, size_t n) {
    if (!st || !buf || n == 0) return -1;
    size_t p = 0;
#define BB_APPEND(...) do { \
        const int _w = snprintf(buf + p, n - p, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= n - p) { buf[p] = '\0'; return (int)p; } \
        p += (size_t)_w; \
    } while (0)
    BB_APPEND("bb:");
    if (flags & BLACKBOX_STATUS_STATE)
        BB_APPEND(" enabled=%d persist=%s filters=0x%X", st->enabled ? 1 : 0, persist_name(st->persist), (unsigned)st->filters);
    if (flags & BLACKBOX_STATUS_COUNTS)
        BB_APPEND(" count=%u dropped=%u inserted=%u flushes=%u", (unsigned)st->count, (unsigned)st->dropped, (unsigned)st->inserted, (unsigned)st->flushes);
    if (flags & BLACKBOX_STATUS_STORAGE)
        BB_APPEND(" bytes=%u/%u(%u%%)", (unsigned)st->bytes, (unsigned)st->pool_sz, (unsigned)st->used_pct);
    if (flags & BLACKBOX_STATUS_TIMING)
        BB_APPEND(" uptime=%ums", (unsigned)st->uptime_ms);
#undef BB_APPEND
    return (int)p;
}

void blackbox_enable(blackbox_handle_t *h, bool on) {
    if (h) h->enabled = on;
}

void blackbox_deinit(blackbox_handle_t *h) {
    if (!h) return;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    if (h->be) { free(h->be); h->be = NULL; }
#endif
    h->cfg = NULL;
}
