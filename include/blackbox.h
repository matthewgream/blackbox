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
 *   BLACKBOX_PERSIST   BLACKBOX_PERSIST_{NONE|FILE|ESP_FLASH|CUSTOM}   (default NONE)
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
#define BLACKBOX_PERSIST_ESP_FLASH  2   /* pool stages, flush appends to a flash partition (esp32)*/
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

/* -------- tags: capped + packed into an integer for fast include/exclude matching ------------- */
/* A tag is at most BLACKBOX_TAG_MAX chars, packed little-endian into blackbox_tag_t, so filter
 * matching is a single integer compare rather than strcmp. 4 chars → uint32, 8 chars → uint64. */
#ifndef BLACKBOX_TAG_MAX
#define BLACKBOX_TAG_MAX 8
#endif
#if BLACKBOX_TAG_MAX <= 4
typedef uint32_t blackbox_tag_t;
#else
typedef uint64_t blackbox_tag_t;
#endif

/* -------- record-type filter (a small fixed-size map of packed tags) -------------------------- */
#ifndef BLACKBOX_FILTER_MAX
#define BLACKBOX_FILTER_MAX 16          /* max tags in the include/exclude list */
#endif
#define BLACKBOX_FILTER_OFF     0u      /* record everything (default)              */
#define BLACKBOX_FILTER_INCLUDE 1u      /* record ONLY the listed tags              */
#define BLACKBOX_FILTER_EXCLUDE 2u      /* record all EXCEPT the listed tags        */

typedef struct {
    uint8_t        mode;                /* BLACKBOX_FILTER_*         */
    uint8_t        count;
    blackbox_tag_t tags[BLACKBOX_FILTER_MAX];
} blackbox_filter_t;

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
    /* Bound the store (initial values; change at run time with blackbox_bound()). Applied to BOTH
     * backends: NONE evicts the oldest record; FILE rotates (active → .old) when the file would
     * exceed max_bytes. 0 = unbounded on that axis. */
    uint32_t max_records;           /* cap on records kept (NONE ring)                            */
    uint32_t max_bytes;             /* cap on stored bytes (NONE) / active-file size (FILE)       */
    uint8_t  generations;           /* FILE at max_bytes: keep this many rotated <path>.N backups  */
                                    /* (0 = overwrite/no backup; N = <path>.1 .. .N, oldest dropped)*/
    const char *persist_arg;        /* FILE: path · ESP_FLASH: partition label · else NULL        */
    bool     enabled;               /* initial gate; toggle at run time via blackbox_enable()     */
} blackbox_config_t;

typedef struct {
    bool     enabled;
    uint8_t  filter_mode;           /* BLACKBOX_FILTER_* in effect                                */
    uint8_t  filter_count;          /* tags in the filter list                                    */
    int      persist;               /* which backend (BLACKBOX_PERSIST_*)                         */
    uint32_t count;                 /* records currently stored                                   */
    uint32_t dropped;               /* records dropped (disabled, full, or encode error)          */
    uint32_t filtered;              /* records skipped by the tag filter                          */
    uint32_t corruptions;           /* times the pool canary was found tainted (and reset)        */
    uint32_t inserted;              /* records accepted since init/clear (monotonic)              */
    uint32_t flushes;               /* number of persist operations                               */
    uint32_t bytes;                 /* bytes currently stored                                     */
    uint32_t pool_sz;               /* pool capacity                                              */
    uint32_t max_records;           /* current bound (0 = unbounded)                              */
    uint32_t max_bytes;             /* current bound (0 = unbounded)                              */
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
    blackbox_filter_t filter;       /* record-type include/exclude filter                         */
    char    *pool_base;             /* record area = cfg->pool + front header (canaries at both ends)*/
    size_t   pool_cap;              /* record capacity = pool_sz - front - back                    */
    size_t   pool_len;              /* record bytes currently staged/stored                        */
    uint32_t count, dropped, filtered, inserted, flushes, stored_bytes, corruptions;
    uint32_t max_records, max_bytes;/* runtime bound (from config; changeable via blackbox_bound) */
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
void blackbox_expire(blackbox_handle_t *h);   /* re-apply the current bound now */

/* Change the store bound at run time (0 = unbounded on that axis); applied immediately. */
void blackbox_bound (blackbox_handle_t *h, uint32_t max_records, uint32_t max_bytes);

bool blackbox_status(blackbox_handle_t *h, blackbox_status_t *out);

/* Render selected sections of a status into buf (see BLACKBOX_STATUS_* flags; ALL for everything).
 * Returns the length written (excluding the NUL), or <0 on error. */
int  blackbox_status_str(const blackbox_status_t *st, uint32_t flags, char *buf, size_t n);

void blackbox_enable(blackbox_handle_t *h, bool on);

/* Check the pool's front+back canaries; if tainted, reset the pool fresh and count a corruption.
 * Returns true if the pool was intact. blackbox_tick calls this periodically. */
bool blackbox_validate(blackbox_handle_t *h);

/* Record-type filter: pack `tag` (≤ BLACKBOX_TAG_MAX chars) and include/exclude it; matching on
 * insert is an integer compare. Default mode is OFF (record everything). */
void blackbox_filter_mode  (blackbox_handle_t *h, uint8_t mode);    /* BLACKBOX_FILTER_*   */
int  blackbox_filter_add   (blackbox_handle_t *h, const char *tag); /* 0 ok, -1 list full  */
void blackbox_filter_remove(blackbox_handle_t *h, const char *tag);
void blackbox_filter_clear (blackbox_handle_t *h);
blackbox_tag_t blackbox_tag_pack(const char *tag);

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

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_CUSTOM
#error "BLACKBOX_PERSIST_CUSTOM backend is not wired yet"
#endif

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
typedef struct { const char *path; } blackbox_file_be_t;
#endif

/* -------- ESP_FLASH backend: a circular append-log over a dedicated esp32 flash partition -------
 * Layout: the partition is a ring of 4 KB sectors. Each sector opens with a header {magic, seq};
 * seq is a monotonic lap counter so recovery can order sectors oldest→newest independent of their
 * physical position. Records pack after the header as [u16 len][payload] (the CSV line, no newline);
 * a record never spans a sector. Appending past a sector's end advances to the next sector (circular),
 * ERASES it first — which evicts its old records, so expiry-by-size is free — and stamps a new header.
 * Recovery on boot scans sector headers for the highest seq (write sector) and lowest (oldest), then
 * walks the write sector to find the free slot (w_off). pull() reads oldest→newest via a byte cursor. */
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_ESP_FLASH
#include "esp_partition.h"

#define BLACKBOX_FLASH_SECTOR    4096u
#define BLACKBOX_FLASH_SEC_MAGIC 0x42584F42u      /* 'BXOB' — sector header magic          */
#define BLACKBOX_FLASH_HDR       8u               /* per-sector header: magic(4) + seq(4)  */
#define BLACKBOX_FLASH_ERASED    0xFFFFu          /* an erased [u16 len] cell (fresh NOR)  */

typedef struct {
    const esp_partition_t *part;
    uint32_t nsec;        /* sectors in the partition                  */
    uint32_t w_off;       /* absolute offset of the next append        */
    uint32_t w_seq;       /* seq stamped on the current write sector   */
    uint32_t oldest_sec;  /* physical index of the oldest live sector  */
    bool     has_data;    /* any record present in the log             */
} blackbox_flash_be_t;

static void blackbox__flash_hdr_write(const esp_partition_t *p, uint32_t sec_off, uint32_t seq) {
    uint8_t hdr[BLACKBOX_FLASH_HDR];
    const uint32_t m = BLACKBOX_FLASH_SEC_MAGIC;
    memcpy(hdr, &m, 4); memcpy(hdr + 4, &seq, 4);
    (void)esp_partition_write(p, sec_off, hdr, BLACKBOX_FLASH_HDR);
}

/* Records in one sector (walk framed cells until an erased/zero len, or the sector fills). */
static uint32_t blackbox__flash_sec_records(const esp_partition_t *p, uint32_t sec_off, uint32_t *bytes, uint32_t *end_off) {
    uint32_t off = sec_off + BLACKBOX_FLASH_HDR;
    const uint32_t end = sec_off + BLACKBOX_FLASH_SECTOR;
    uint32_t n = 0, b = 0;
    while (off + 2u <= end) {
        uint16_t len = 0;
        (void)esp_partition_read(p, off, &len, 2);
        if (len == BLACKBOX_FLASH_ERASED || len == 0 || off + 2u + (uint32_t)len > end) break;
        n++; b += (uint32_t)len; off += 2u + (uint32_t)len;
    }
    if (bytes)   *bytes = b;
    if (end_off) *end_off = off;                  /* first free slot in this sector */
    return n;
}

/* Boot recovery: find the write sector (max seq) + oldest sector (min seq), the write offset, and a
 * live record/byte count. Virgin partition → lay down sector 0. Sets h->count / h->stored_bytes. */
static void blackbox__flash_recover(blackbox_handle_t *h) {
    blackbox_flash_be_t *be = (blackbox_flash_be_t *)h->be;
    bool any = false;
    uint32_t best_seq = 0, best_end = BLACKBOX_FLASH_HDR;
    uint32_t min_seq = 0, min_sec = 0, total = 0, bytes = 0;
    for (uint32_t s = 0; s < be->nsec; s++) {
        const uint32_t off = s * BLACKBOX_FLASH_SECTOR;
        uint32_t magic = 0, seq = 0;
        (void)esp_partition_read(be->part, off, &magic, 4);
        (void)esp_partition_read(be->part, off + 4, &seq, 4);
        if (magic != BLACKBOX_FLASH_SEC_MAGIC) continue;
        uint32_t sb = 0, se = 0;
        total += blackbox__flash_sec_records(be->part, off, &sb, &se);
        bytes += sb;
        if (!any) { any = true; best_seq = min_seq = seq; min_sec = s; best_end = se; }
        else {
            if (seq > best_seq) { best_seq = seq; best_end = se; }
            if (seq < min_seq)  { min_seq = seq;  min_sec = s; }
        }
    }
    if (!any) {                                   /* virgin partition — lay down sector 0 */
        be->w_seq = 1; be->oldest_sec = 0; be->has_data = false;
        (void)esp_partition_erase_range(be->part, 0, BLACKBOX_FLASH_SECTOR);
        blackbox__flash_hdr_write(be->part, 0, be->w_seq);
        be->w_off = BLACKBOX_FLASH_HDR;
        return;
    }
    be->w_seq      = best_seq;
    be->oldest_sec = min_sec;
    be->w_off      = best_end;                     /* end_off from flash_sec_records is absolute */
    be->has_data   = (total > 0);
    h->count       = total;
    h->stored_bytes = bytes;
}

/* Append one framed record; on sector advance, evict the sector being reused (adjust count/bytes). */
static int blackbox__flash_append(blackbox_handle_t *h, const char *payload, uint16_t len) {
    blackbox_flash_be_t *be = (blackbox_flash_be_t *)h->be;
    const uint32_t rec = 2u + (uint32_t)len;
    if (rec > BLACKBOX_FLASH_SECTOR - BLACKBOX_FLASH_HDR) return -1;   /* one record must fit a sector */
    /* Current sector = the one holding the last written byte (w_off-1); this makes an exactly-full
       sector (w_off == sector end) resolve to that sector and advance, instead of aliasing to the
       next sector's header. w_off is always >= BLACKBOX_FLASH_HDR, so w_off-1 never underflows. */
    const uint32_t sec_off = (be->w_off - 1u) & ~(BLACKBOX_FLASH_SECTOR - 1u);
    if (be->w_off + rec > sec_off + BLACKBOX_FLASH_SECTOR) {           /* no room → advance a sector */
        const uint32_t cur = sec_off / BLACKBOX_FLASH_SECTOR;
        const uint32_t nxt = (cur + 1u) % be->nsec;
        const uint32_t nxt_off = nxt * BLACKBOX_FLASH_SECTOR;
        if (nxt == be->oldest_sec) {                                  /* reusing the oldest → evict it */
            uint32_t eb = 0;
            const uint32_t er = blackbox__flash_sec_records(be->part, nxt_off, &eb, NULL);
            h->count        = (h->count > er) ? h->count - er : 0;
            h->stored_bytes = (h->stored_bytes > eb) ? h->stored_bytes - eb : 0;
            be->oldest_sec  = (be->oldest_sec + 1u) % be->nsec;
        }
        if (esp_partition_erase_range(be->part, nxt_off, BLACKBOX_FLASH_SECTOR) != ESP_OK) return -1;
        be->w_seq += 1u;
        blackbox__flash_hdr_write(be->part, nxt_off, be->w_seq);
        be->w_off = nxt_off + BLACKBOX_FLASH_HDR;
    }
    if (esp_partition_write(be->part, be->w_off, &len, 2) != ESP_OK) return -1;
    if (len && esp_partition_write(be->part, be->w_off + 2u, payload, len) != ESP_OK) return -1;
    be->w_off += rec;
    be->has_data = true;
    return 0;
}
#endif /* BLACKBOX_PERSIST_ESP_FLASH */

/* Pool canaries: a survivability header at the front and a guard word at the back. Both hold the
 * magic; the front also holds the record length. Purposes: (1) on init, an intact pool that survived
 * (RTC across sleep/fault) is ADOPTED, a garbage/first-boot pool is started fresh; (2) the front+back
 * magics are stack-canary-style guard bands — an over/under-run taints one and blackbox_validate()
 * catches it. Works on any platform. */
#define BLACKBOX_POOL_MAGIC  0xB1ACB0C5u
#define BLACKBOX_POOL_FRONT  8u    /* magic(4) + len(4) */
#define BLACKBOX_POOL_BACK   4u    /* magic(4)          */

static uint32_t blackbox__rd32(const char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     blackbox__wr32(char *p, uint32_t v) { memcpy(p, &v, 4); }

static bool blackbox__pool_intact(const blackbox_handle_t *h) {
    return blackbox__rd32(h->cfg->pool) == BLACKBOX_POOL_MAGIC
        && blackbox__rd32(h->cfg->pool + h->cfg->pool_sz - BLACKBOX_POOL_BACK) == BLACKBOX_POOL_MAGIC;
}
static void blackbox__pool_stamp(blackbox_handle_t *h) {   /* (re)write both canaries + len — fresh */
    blackbox__wr32(h->cfg->pool, BLACKBOX_POOL_MAGIC);
    blackbox__wr32(h->cfg->pool + 4, (uint32_t)h->pool_len);
    blackbox__wr32(h->cfg->pool + h->cfg->pool_sz - BLACKBOX_POOL_BACK, BLACKBOX_POOL_MAGIC);
}
static void blackbox__pool_sync(blackbox_handle_t *h) {    /* update len only (leave canaries intact for taint detection) */
    blackbox__wr32(h->cfg->pool + 4, (uint32_t)h->pool_len);
}

static uint8_t blackbox__used_pct(const blackbox_handle_t *h) {
    if (!h->pool_cap) return 0;
    return (uint8_t)((h->pool_len * 100u) / h->pool_cap);
}

blackbox_tag_t blackbox_tag_pack(const char *tag) {
    blackbox_tag_t t = 0;
    for (int i = 0; i < BLACKBOX_TAG_MAX && tag[i]; i++)
        t |= (blackbox_tag_t)(unsigned char)tag[i] << (8 * i);
    return t;
}

static bool blackbox__filter_pass(const blackbox_handle_t *h, blackbox_tag_t t) {
    if (h->filter.mode == BLACKBOX_FILTER_OFF) return true;
    bool found = false;
    for (uint8_t i = 0; i < h->filter.count; i++)
        if (h->filter.tags[i] == t) { found = true; break; }
    return (h->filter.mode == BLACKBOX_FILTER_INCLUDE) ? found : !found;
}

static const char *blackbox__filter_name(uint8_t m) {
    switch (m) {
    case BLACKBOX_FILTER_INCLUDE: return "include";
    case BLACKBOX_FILTER_EXCLUDE: return "exclude";
    default:                      return "off";
    }
}

/* Count whole lines currently in the record area (adopt + FILE rotation). */
static uint32_t blackbox__pool_lines(const blackbox_handle_t *h) {
    uint32_t n = 0;
    for (size_t i = 0; i < h->pool_len; i++)
        if (h->pool_base[i] == '\n') n++;
    return n;
}

#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_NONE
/* Evict the single oldest whole line from the record area (NONE ring / bound). */
static void blackbox__evict_oldest(blackbox_handle_t *h) {
    if (h->pool_len == 0) return;
    const char *nl = memchr(h->pool_base, '\n', h->pool_len);
    const size_t evict = nl ? (size_t)(nl - h->pool_base) + 1u : h->pool_len;
    memmove(h->pool_base, h->pool_base + evict, h->pool_len - evict);
    h->pool_len -= evict;
    if (h->count) h->count--;
    h->stored_bytes -= (h->stored_bytes >= (uint32_t)evict) ? (uint32_t)evict : h->stored_bytes;
    blackbox__pool_sync(h);
}
/* Enforce the record/byte bound by evicting the oldest lines (NONE). 0 on an axis = unbounded. */
static void blackbox__enforce_bound(blackbox_handle_t *h) {
    while (h->pool_len > 0 &&
           ((h->max_records && h->count > h->max_records) ||
            (h->max_bytes && h->stored_bytes > h->max_bytes)))
        blackbox__evict_oldest(h);
}
#endif

/* Append one assembled line (ln bytes, includes the trailing '\n') into the store. */
static int blackbox__store_append(blackbox_handle_t *h, const char *line, size_t ln) {
    if (ln == 0 || ln > h->pool_cap)
        return -1;
#if BLACKBOX_PERSIST != BLACKBOX_PERSIST_NONE
    if (h->pool_len + ln > h->pool_cap)         /* stage full → drain to the backend first */
        if (blackbox_flush(h) != 0)
            return -1;
    if (h->pool_len + ln > h->pool_cap)         /* still no room (shouldn't happen) */
        return -1;
#else /* PERSIST_NONE — the record area is a ring; evict whole oldest lines to make room */
    while (h->pool_len + ln > h->pool_cap && h->pool_len > 0)
        blackbox__evict_oldest(h);
    if (h->pool_len + ln > h->pool_cap)
        return -1;
#endif
    memcpy(h->pool_base + h->pool_len, line, ln);
    h->pool_len += ln;
    h->count++;
    h->stored_bytes += (uint32_t)ln;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_NONE
    blackbox__enforce_bound(h);                 /* honour max_records / max_bytes (RAM ring) */
#endif
    blackbox__pool_sync(h);
    return 0;
}

int blackbox_init(blackbox_handle_t *h, const blackbox_config_t *cfg) {
    if (!h || !cfg || !cfg->pool || cfg->pool_sz < BLACKBOX_LINE_MAX + BLACKBOX_POOL_FRONT + BLACKBOX_POOL_BACK)
        return -1;
    memset(h, 0, sizeof(*h));
    h->cfg = cfg;
    h->enabled = cfg->enabled;
    h->max_records = cfg->max_records;
    h->max_bytes = cfg->max_bytes;
    h->pool_base = cfg->pool + BLACKBOX_POOL_FRONT;
    h->pool_cap = cfg->pool_sz - BLACKBOX_POOL_FRONT - BLACKBOX_POOL_BACK;
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    blackbox_file_be_t *be = (blackbox_file_be_t *)calloc(1, sizeof(*be));
    if (!be) return -1;
    be->path = cfg->persist_arg ? cfg->persist_arg : "blackbox.csv";
    /* The file is NOT created here. It used to be opened for append just to prove the path was
       writable, which left an empty file behind on every run -- including runs with the recorder
       disabled, which never write anything at all. The flush path opens with "a" and creates it
       there, so the file now appears on the first record actually persisted and not before.
       The cost is that an unwritable path is reported by the first flush rather than by init. */
    h->be = be;
#elif BLACKBOX_PERSIST == BLACKBOX_PERSIST_ESP_FLASH
    blackbox_flash_be_t *fbe = (blackbox_flash_be_t *)calloc(1, sizeof(*fbe));
    if (!fbe) return -1;
    fbe->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                         (esp_partition_subtype_t)0x40,   /* iotdata diag subtype */
                                         cfg->persist_arg ? cfg->persist_arg : "diag");
    if (!fbe->part || fbe->part->size < BLACKBOX_FLASH_SECTOR) { free(fbe); return -1; }
    fbe->nsec = (uint32_t)(fbe->part->size / BLACKBOX_FLASH_SECTOR);
    h->be = fbe;
    blackbox__flash_recover(h);                 /* find write/oldest cursor + live count */
#endif
    /* Adopt a surviving pool (intact canaries — e.g. RTC RAM across deep sleep / a fault), else
     * start fresh (normal RAM boots to garbage → canaries don't match → fresh). */
    if (blackbox__pool_intact(h)) {
        const uint32_t len = blackbox__rd32(cfg->pool + 4);
        if ((size_t)len <= h->pool_cap) {
            h->pool_len = len;
            h->count += blackbox__pool_lines(h);
            h->stored_bytes += (uint32_t)h->pool_len;
        } else {
            h->pool_len = 0;
            blackbox__pool_stamp(h);
        }
    } else {
        h->pool_len = 0;
        blackbox__pool_stamp(h);
    }
    return 0;
}

int blackbox_insert(blackbox_handle_t *h, const blackbox_struct_config_t *sc, const void *data) {
    if (!h || !sc || !sc->encode) return -1;
    if (!h->enabled) { h->dropped++; return 0; }
    if (!blackbox__filter_pass(h, blackbox_tag_pack(sc->tag))) { h->filtered++; return 0; }

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

bool blackbox_validate(blackbox_handle_t *h) {
    if (!h) return false;
    if (blackbox__pool_intact(h)) return true;
    /* A guard band was trampled — buffer over/under-run, or RAM corruption. Don't trust the
     * contents: reset to a clean, freshly-stamped pool and count the event. */
    h->corruptions++;
    h->pool_len = 0;
    h->count = 0;
    h->stored_bytes = 0;
    blackbox__pool_stamp(h);
    return false;
}

void blackbox_tick(blackbox_handle_t *h, uint32_t dt_ms) {
    if (!h) return;
    (void)blackbox_validate(h);     /* guard-band taint check — cheap: two 4-byte reads */
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
    /* Byte bound: rotate the active file (→ .old) before it would exceed max_bytes. stored_bytes
       tracks active-file + staged, so this fires once the pending flush would push it over. Total
       footprint stays under ~2×max_bytes (active + one backup); a rename, not a rewrite. */
    if (h->max_bytes && h->stored_bytes > h->max_bytes) {
        const uint8_t gens = h->cfg->generations;
        if (gens == 0) {
            FILE *tf = fopen(be->path, "w");    /* overwrite — no backup (~1× footprint) */
            if (tf) (void)fclose(tf);
        } else {                                /* generational: shift .N-1→.N … .1→.2, active→.1 */
            const size_t sz = strlen(be->path) + 6;   /* path + ".NNN" + NUL */
            char *from = (char *)malloc(sz), *to = (char *)malloc(sz);
            if (from && to) {
                for (int g = (int)gens - 1; g >= 1; g--) {   /* oldest (.gens) is overwritten/dropped */
                    (void)snprintf(from, sz, "%s.%d", be->path, g);
                    (void)snprintf(to, sz, "%s.%d", be->path, g + 1);
                    (void)rename(from, to);     /* ENOENT if .g absent — harmless */
                }
                (void)snprintf(to, sz, "%s.1", be->path);
                (void)rename(be->path, to);     /* active → .1 */
            }
            free(from); free(to);
        }
        h->stored_bytes = (uint32_t)h->pool_len;   /* the active file is now empty */
        h->count = blackbox__pool_lines(h);
    }
    FILE *f = fopen(be->path, "a");
    if (!f) return -1;
    const size_t w = fwrite(h->pool_base, 1, h->pool_len, f);
    (void)fflush(f);
    (void)fclose(f);
    if (w != h->pool_len) return -1;
    h->pool_len = 0;                            /* staged bytes are now durable in the file */
    h->flushes++;
    blackbox__pool_sync(h);
#elif BLACKBOX_PERSIST == BLACKBOX_PERSIST_ESP_FLASH
    if (h->pool_len == 0) return 0;
    /* Drain the staged pool to the flash log, one framed record per CSV line. Sector eviction
       (expiry-by-size) happens inside blackbox__flash_append as the log wraps. */
    const char *p = h->pool_base;
    size_t left = h->pool_len;
    while (left > 0) {
        const char *nl = (const char *)memchr(p, '\n', left);
        size_t ll = nl ? (size_t)(nl - p) : left;          /* payload length, no newline */
        if (ll > 0xFFFFu) ll = 0xFFFFu;                    /* lines are ≤ LINE_MAX; clamp defensively */
        if (blackbox__flash_append(h, p, (uint16_t)ll) != 0) return -1;
        const size_t adv = nl ? ll + 1u : ll;              /* consume the newline too */
        p += adv; left -= adv;
    }
    h->pool_len = 0;                            /* staged bytes are now durable in flash */
    h->flushes++;
    blackbox__pool_sync(h);
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
#elif BLACKBOX_PERSIST == BLACKBOX_PERSIST_ESP_FLASH
    blackbox_flash_be_t *be = (blackbox_flash_be_t *)h->be;
    if (*cursor == 0) {
        (void)blackbox_flush(h);                /* make staged records visible to the read */
        if (!be->has_data) return 0;
        *cursor = (size_t)be->oldest_sec * BLACKBOX_FLASH_SECTOR + BLACKBOX_FLASH_HDR;
    }
    for (;;) {                                  /* walk oldest→newest, skipping sector headers/gaps */
        if ((uint32_t)*cursor == be->w_off) return 0;   /* reached the write head → done */
        const uint32_t sec_off = (uint32_t)(*cursor - 1u) & ~(BLACKBOX_FLASH_SECTOR - 1u);  /* sector of the last byte */
        const uint32_t end = sec_off + BLACKBOX_FLASH_SECTOR;
        uint16_t len = 0;
        if ((uint32_t)*cursor + 2u <= end)
            (void)esp_partition_read(be->part, (uint32_t)*cursor, &len, 2);
        if ((uint32_t)*cursor + 2u > end || len == BLACKBOX_FLASH_ERASED || len == 0
            || (uint32_t)*cursor + 2u + (uint32_t)len > end) {   /* no more records here → next sector in seq order */
            const uint32_t nxt = (sec_off / BLACKBOX_FLASH_SECTOR + 1u) % be->nsec;
            if (nxt == be->oldest_sec) return 0;         /* wrapped all the way round → done */
            *cursor = (size_t)nxt * BLACKBOX_FLASH_SECTOR + BLACKBOX_FLASH_HDR;
            continue;
        }
        const uint32_t copy = ((uint32_t)len < (uint32_t)(n - 1)) ? (uint32_t)len : (uint32_t)(n - 1);
        (void)esp_partition_read(be->part, (uint32_t)*cursor + 2u, line, copy);
        line[copy] = '\0';
        *cursor += 2u + (size_t)len;
        return (int)strlen(line);               /* stored without a newline — nothing to strip */
    }
#else
    if (*cursor >= h->pool_len) return 0;
    const char *base = h->pool_base + *cursor;
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
    blackbox__pool_sync(h);
#if BLACKBOX_PERSIST == BLACKBOX_PERSIST_FILE
    blackbox_file_be_t *be = (blackbox_file_be_t *)h->be;
    FILE *f = fopen(be->path, "w");             /* truncate */
    if (f) (void)fclose(f);
#elif BLACKBOX_PERSIST == BLACKBOX_PERSIST_ESP_FLASH
    blackbox_flash_be_t *be = (blackbox_flash_be_t *)h->be;
    (void)esp_partition_erase_range(be->part, 0, (size_t)be->nsec * BLACKBOX_FLASH_SECTOR);
    be->w_seq = 1; be->oldest_sec = 0; be->has_data = false;
    blackbox__flash_hdr_write(be->part, 0, be->w_seq);   /* re-lay sector 0 */
    be->w_off = BLACKBOX_FLASH_HDR;
#endif
}

void blackbox_expire(blackbox_handle_t *h) {
    if (!h) return;
#if BLACKBOX_PERSIST != BLACKBOX_PERSIST_NONE
    (void)blackbox_flush(h);        /* FILE: rotation enforces the byte bound. ESP_FLASH: the log
                                       self-evicts by sector as it wraps — flush just persists staged. */
#else
    blackbox__enforce_bound(h);     /* NONE: evict oldest to fit max_records / max_bytes */
#endif
}

void blackbox_bound(blackbox_handle_t *h, uint32_t max_records, uint32_t max_bytes) {
    if (!h) return;
    h->max_records = max_records;
    h->max_bytes = max_bytes;
    blackbox_expire(h);             /* apply the new bound immediately */
}

bool blackbox_status(blackbox_handle_t *h, blackbox_status_t *out) {
    if (!h || !out) return false;
    out->enabled      = h->enabled;
    out->filter_mode  = h->filter.mode;
    out->filter_count = h->filter.count;
    out->persist      = BLACKBOX_PERSIST;
    out->count        = h->count;
    out->dropped      = h->dropped;
    out->filtered     = h->filtered;
    out->inserted     = h->inserted;
    out->flushes  = h->flushes;
    out->corruptions = h->corruptions;
    out->bytes    = h->stored_bytes;
    out->pool_sz  = (uint32_t)h->pool_cap;      /* usable record capacity (excludes canary bands) */
    out->max_records = h->max_records;
    out->max_bytes   = h->max_bytes;
    out->used_pct = blackbox__used_pct(h);
    out->uptime_ms = h->uptime_ms;
    return true;
}

static const char *blackbox__persist_name(int p) {
    switch (p) {
    case BLACKBOX_PERSIST_NONE:      return "none";
    case BLACKBOX_PERSIST_FILE:      return "file";
    case BLACKBOX_PERSIST_ESP_FLASH: return "esp-flash";
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
        BLACKBOX__APPEND(" enabled=%d persist=%s filter=%s(%u)", st->enabled ? 1 : 0, blackbox__persist_name(st->persist), blackbox__filter_name(st->filter_mode), (unsigned)st->filter_count);
    if (flags & BLACKBOX_STATUS_COUNTS)
        BLACKBOX__APPEND(" count=%u dropped=%u filtered=%u inserted=%u flushes=%u corruptions=%u", (unsigned)st->count, (unsigned)st->dropped, (unsigned)st->filtered, (unsigned)st->inserted, (unsigned)st->flushes, (unsigned)st->corruptions);
    if (flags & BLACKBOX_STATUS_STORAGE)
        BLACKBOX__APPEND(" bytes=%u/%u(%u%%) bound=%urec/%ub", (unsigned)st->bytes, (unsigned)st->pool_sz, (unsigned)st->used_pct, (unsigned)st->max_records, (unsigned)st->max_bytes);
    if (flags & BLACKBOX_STATUS_TIMING)
        BLACKBOX__APPEND(" uptime=%ums", (unsigned)st->uptime_ms);
#undef BLACKBOX__APPEND
    return (int)p;
}

void blackbox_enable(blackbox_handle_t *h, bool on) {
    if (h) h->enabled = on;
}

void blackbox_filter_mode(blackbox_handle_t *h, uint8_t mode) {
    if (h) h->filter.mode = mode;
}
int blackbox_filter_add(blackbox_handle_t *h, const char *tag) {
    if (!h || !tag) return -1;
    const blackbox_tag_t t = blackbox_tag_pack(tag);
    for (uint8_t i = 0; i < h->filter.count; i++)
        if (h->filter.tags[i] == t) return 0;           /* already present */
    if (h->filter.count >= BLACKBOX_FILTER_MAX) return -1;
    h->filter.tags[h->filter.count++] = t;
    return 0;
}
void blackbox_filter_remove(blackbox_handle_t *h, const char *tag) {
    if (!h || !tag) return;
    const blackbox_tag_t t = blackbox_tag_pack(tag);
    for (uint8_t i = 0; i < h->filter.count; i++)
        if (h->filter.tags[i] == t) {
            h->filter.tags[i] = h->filter.tags[--h->filter.count];  /* swap-remove */
            return;
        }
}
void blackbox_filter_clear(blackbox_handle_t *h) {
    if (h) { h->filter.count = 0; h->filter.mode = BLACKBOX_FILTER_OFF; }
}

void blackbox_deinit(blackbox_handle_t *h) {
    if (!h) return;
#if BLACKBOX_PERSIST != BLACKBOX_PERSIST_NONE
    if (h->be) { free(h->be); h->be = NULL; }   /* FILE + ESP_FLASH allocate a backend struct */
#endif
    h->cfg = NULL;
}

#endif /* BLACKBOX_IMPLEMENTATION */

#endif /* BLACKBOX_H */
