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

#endif /* BLACKBOX_H */
