/*
 * blackbox — a generic, compile-time-configured structured-record recorder for embedded + host.
 *
 * You fill a small typed C struct; blackbox encodes it to a compact CSV line (via a per-type
 * descriptor you supply), stamps a generic `clock` column, and appends it through a compile-time
 * selected backend. Records can be flushed, pulled back, cleared and expired. The library is
 * struct-agnostic — it stores tagged CSV lines and knows nothing about your record types.
 *
 * See DESIGN.md. Configure with #defines BEFORE including this header (typically from a project
 * adapter, e.g. iotdata's iotdata_blackbox.h):
 *
 *   BLACKBOX_PERSIST   BLACKBOX_PERSIST_{NONE|FILE|MDS_FLASH|CUSTOM}   (default NONE)
 *   BLACKBOX_CLOCK     name of  int fn(char *out, size_t n)  writing the clock column  (required)
 *   BLACKBOX_COMPRESS  BLACKBOX_COMPRESS_NONE                          (dormant hook, default NONE)
 *
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */
#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* -------- compile-time backend selection ------------------------------------------------------ */

#define BLACKBOX_PERSIST_NONE       0   /* the RAM pool IS the store (a ring); flush is a no-op   */
#define BLACKBOX_PERSIST_FILE       1   /* pool stages, flush appends to a file (host)            */
#define BLACKBOX_PERSIST_MDS_FLASH  2   /* pool stages, flush appends to a flash partition (esp32)*/
#define BLACKBOX_PERSIST_CUSTOM     3   /* project supplies the ops                               */
#ifndef BLACKBOX_PERSIST
#define BLACKBOX_PERSIST BLACKBOX_PERSIST_NONE
#endif

#define BLACKBOX_COMPRESS_NONE      0   /* dormant hook — records are stored as plain CSV lines   */
#ifndef BLACKBOX_COMPRESS
#define BLACKBOX_COMPRESS BLACKBOX_COMPRESS_NONE
#endif

#ifndef BLACKBOX_CLOCK
#error "define BLACKBOX_CLOCK to your clock hook: int BLACKBOX_CLOCK(char *out, size_t n)"
#endif
int BLACKBOX_CLOCK(char *out, size_t n);   /* forward-declare the configured hook */

/* Longest single record line the library will assemble (tag,clock,payload + '\n'). */
#ifndef BLACKBOX_LINE_MAX
#define BLACKBOX_LINE_MAX 160
#endif

/* -------- record descriptor (one per record type; the project defines these) ------------------ */

typedef struct blackbox_struct_config {
    const char *tag;                                                                  /* "LC", "MSH", … */
    int (*encode)(const struct blackbox_struct_config *sc, const void *data, char *out, size_t outlen);
    int (*decode)(const struct blackbox_struct_config *sc, const char *in, size_t inlen, void *data);
} blackbox_struct_config_t;

/* -------- runtime config + handle ------------------------------------------------------------- */

typedef enum {
    BLACKBOX_FLUSH_MANUAL = 0,      /* persist only on an explicit blackbox_flush()               */
    BLACKBOX_FLUSH_BATCH_SIZE,      /* flush when the pool is near full                           */
    BLACKBOX_FLUSH_BATCH_TIME,      /* flush every cfg.flush_ms (driven by blackbox_tick)         */
    BLACKBOX_FLUSH_PRESLEEP,        /* like MANUAL — caller flushes before deep sleep             */
    BLACKBOX_FLUSH_WRITE_THROUGH,   /* persist every insert immediately (durable, for debugging)  */
} blackbox_flush_t;

typedef struct {
    char    *pool;                  /* caller-provided RAM (static | RTC_NOINIT | heap)           */
    size_t   pool_sz;
    blackbox_flush_t flush;
    uint32_t flush_ms;              /* for BATCH_TIME                                             */
    uint32_t expire_limit;          /* max stored records (0 = backend default / unbounded-ish)   */
    const char *persist_arg;        /* FILE: path · MDS_FLASH: partition label · else NULL        */
    bool     enabled;               /* initial gate; toggle at run time via blackbox_enable()     */
} blackbox_config_t;

typedef struct {
    bool     enabled;
    uint32_t filters;               /* reserved: per-record-type filter mask (0 = none yet)       */
    int      persist;               /* which backend (BLACKBOX_PERSIST_*)                         */
    uint32_t count;                 /* records currently stored                                   */
    uint32_t dropped;               /* records dropped (disabled, full, or encode error)          */
    uint32_t inserted;              /* records accepted since init/clear (monotonic)              */
    uint32_t flushes;               /* number of persist operations                               */
    uint32_t bytes;                 /* bytes currently stored                                     */
    uint32_t pool_sz;               /* pool capacity                                              */
    uint8_t  used_pct;              /* store fullness, 0..100                                      */
    uint32_t uptime_ms;             /* ms accumulated via blackbox_tick since init                */
} blackbox_status_t;

/* Section flags for blackbox_status_str(). */
#define BLACKBOX_STATUS_STATE    0x01u   /* enabled, persist, filters      */
#define BLACKBOX_STATUS_COUNTS   0x02u   /* count, dropped, inserted, flush*/
#define BLACKBOX_STATUS_STORAGE  0x04u   /* bytes, used%, pool_sz          */
#define BLACKBOX_STATUS_TIMING   0x08u   /* uptime                         */
#define BLACKBOX_STATUS_ALL      0x0Fu

typedef struct {
    const blackbox_config_t *cfg;
    bool     enabled;
    size_t   pool_len;              /* bytes currently staged/stored in the pool                  */
    uint32_t count, dropped, inserted, flushes, stored_bytes;
    uint32_t flush_timer_ms;        /* accumulated by blackbox_tick, reset on a BATCH_TIME flush  */
    uint32_t uptime_ms;             /* accumulated by blackbox_tick, monotonic                    */
    void    *be;                    /* backend state (opaque; set by the backend at init)         */
} blackbox_handle_t;

/* -------- lifecycle API ----------------------------------------------------------------------- */

/* Returns 0 on success, <0 on error. */
int  blackbox_init  (blackbox_handle_t *h, const blackbox_config_t *cfg);

/* Encode `data` via `sc`, stamp tag+clock, stage into the pool (or persist if WRITE_THROUGH). */
int  blackbox_insert(blackbox_handle_t *h, const blackbox_struct_config_t *sc, const void *data);

/* Drive time-based flush; call periodically (pass the ms since the previous tick). */
void blackbox_tick  (blackbox_handle_t *h, uint32_t dt_ms);

/* Persist the staged pool to the backend now ("save"). No-op for PERSIST_NONE. */
int  blackbox_flush (blackbox_handle_t *h);

/* Iterate stored records as raw CSV lines ("load"). Start with *cursor = 0; returns line length
 * (>0) and advances *cursor, 0 at end, <0 on error. */
int  blackbox_pull  (blackbox_handle_t *h, size_t *cursor, char *line, size_t n);

void blackbox_clear (blackbox_handle_t *h);
void blackbox_expire(blackbox_handle_t *h);
bool blackbox_status(blackbox_handle_t *h, blackbox_status_t *out);

/* Render selected sections of a status into buf (see BLACKBOX_STATUS_* flags; ALL for everything).
 * Returns the length written (excluding the NUL), or <0 on error. */
int  blackbox_status_str(const blackbox_status_t *st, uint32_t flags, char *buf, size_t n);

void blackbox_enable(blackbox_handle_t *h, bool on);
void blackbox_deinit(blackbox_handle_t *h);

/* ==================================================================================================
 * Implementation — single-header / unity style.  In ONE translation unit:
 *     #define BLACKBOX_IMPLEMENTATION
 *     #include "blackbox.h"
 * (iotdata's adapter does this for the app's unity TU; src/blackbox.c does it for a separate-TU build.)
 * The store is the caller's pool holding newline-terminated CSV lines back to back:
 *   PERSIST_NONE : the pool IS the store — a ring; inserts evict the oldest line when full.
 *   PERSIST_FILE : the pool STAGES lines; flush() appends them to a file and empties the pool.
 * pull()'s cursor is a byte offset (into the pool for NONE, into the file for FILE).
 * ================================================================================================== */
#ifdef BLACKBOX_IMPLEMENTATION

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
typedef struct { const char *path; } blackbox_file_be_t;
#endif

static uint8_t blackbox__used_pct(const blackbox_handle_t *h) {
    if (!h->cfg->pool_sz) return 0;
    return (uint8_t)((h->pool_len * 100u) / h->cfg->pool_sz);
}

/* Append one assembled line (ln bytes, includes the trailing '\n') into the store. */
static int blackbox__store_append(blackbox_handle_t *h, const char *line, size_t ln) {
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

int blackbox_init(blackbox_handle_t *h, const blackbox_config_t *cfg) {
    if (!h || !cfg || !cfg->pool || cfg->pool_sz < BLACKBOX_LINE_MAX)
        return -1;
    memset(h, 0, sizeof(*h));
    h->cfg = cfg;
    h->enabled = cfg->enabled;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    blackbox_file_be_t *be = (blackbox_file_be_t *)calloc(1, sizeof(*be));
    if (!be) return -1;
    be->path = cfg->persist_arg ? cfg->persist_arg : "blackbox.csv";
    FILE *f = fopen(be->path, "a");             /* ensure it exists / is appendable */
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

    if (blackbox__store_append(h, line, (size_t)ln) != 0) { h->dropped++; return -1; }
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
    blackbox_file_be_t *be = (blackbox_file_be_t *)h->be;
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
    blackbox_file_be_t *be = (blackbox_file_be_t *)h->be;
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
    size_t len = strlen(line);                  /* strip the trailing newline for the caller */
    if (len && line[len - 1] == '\n') line[--len] = '\0';
    return (int)len;
}

void blackbox_clear(blackbox_handle_t *h) {
    if (!h) return;
    h->pool_len = 0;
    h->count = 0;
    h->stored_bytes = 0;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    blackbox_file_be_t *be = (blackbox_file_be_t *)h->be;
    FILE *f = fopen(be->path, "w");             /* truncate */
    if (f) (void)fclose(f);
#endif
}

void blackbox_expire(blackbox_handle_t *h) {
    if (!h || h->cfg->expire_limit == 0) return;
#if BLACKBOX_PERSIST != BLACKBOX_PERSIST_FILE
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
    out->used_pct = blackbox__used_pct(h);
    out->uptime_ms = h->uptime_ms;
    return true;
}

static const char *blackbox__persist_name(int p) {
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
#define BLACKBOX__APPEND(...) do { \
        const int _w = snprintf(buf + p, n - p, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= n - p) { buf[p] = '\0'; return (int)p; } \
        p += (size_t)_w; \
    } while (0)
    BLACKBOX__APPEND("bb:");
    if (flags & BLACKBOX_STATUS_STATE)
        BLACKBOX__APPEND(" enabled=%d persist=%s filters=0x%X", st->enabled ? 1 : 0, blackbox__persist_name(st->persist), (unsigned)st->filters);
    if (flags & BLACKBOX_STATUS_COUNTS)
        BLACKBOX__APPEND(" count=%u dropped=%u inserted=%u flushes=%u", (unsigned)st->count, (unsigned)st->dropped, (unsigned)st->inserted, (unsigned)st->flushes);
    if (flags & BLACKBOX_STATUS_STORAGE)
        BLACKBOX__APPEND(" bytes=%u/%u(%u%%)", (unsigned)st->bytes, (unsigned)st->pool_sz, (unsigned)st->used_pct);
    if (flags & BLACKBOX_STATUS_TIMING)
        BLACKBOX__APPEND(" uptime=%ums", (unsigned)st->uptime_ms);
#undef BLACKBOX__APPEND
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

#endif /* BLACKBOX_IMPLEMENTATION */

#endif /* BLACKBOX_H */
