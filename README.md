# blackbox

A small, generic, **compile-time-configured** structured-record recorder for embedded + host — a
"black box" flight-recorder for your firmware. You fill a typed C struct; blackbox encodes it to a
compact CSV line (via a per-type descriptor you supply), stamps a generic `clock` column, and appends
it through a compile-time-selected backend. Records can be flushed, pulled back, cleared, and expired.

The library is **struct-agnostic** — it stores tagged CSV lines and knows nothing about your record
types. A companion `csv2json` tool turns stored CSV into JSON using your record header for the keys.

See [DESIGN.md](DESIGN.md) for the full design.

## Build & test
```
make test          # runs the C library test against PERSIST_NONE and PERSIST_FILE
make csv2json-test # runs csv2json over the example fixtures
```

## Using it (C)
blackbox is a **single-header** library. Configure with `#define`s **before** including it, and in
**one** translation unit also define `BLACKBOX_IMPLEMENTATION` (typically the app's unity TU — a
project adapter does this):
```c
#define BLACKBOX_PERSIST         BLACKBOX_PERSIST_FILE   /* NONE | FILE | MDS_FLASH(esp32,P3) | CUSTOM */
#define BLACKBOX_CLOCK           my_clock                /* int my_clock(char *out, size_t n)          */
#define BLACKBOX_IMPLEMENTATION                          /* in ONE TU only                             */
#include "blackbox.h"
```
Other translation units just `#include "blackbox.h"` for the declarations. (`src/blackbox.c` is a
ready-made implementation TU for builds that prefer a standalone object over unity.)
Describe each record type (struct + encoder + a descriptor). The descriptor carries the encoder, so
`blackbox_insert` stays generic:
```c
typedef struct { uint8_t reason; uint16_t bootcount; } lc_event_t;
static int enc_lc(const blackbox_struct_config_t *sc, const void *d, char *o, size_t n) {
    (void)sc; const lc_event_t *e = d; return snprintf(o, n, "%u,%u", e->reason, e->bootcount);
}
static const blackbox_struct_config_t bb_lc = { "LC", enc_lc, NULL };
```
Lifecycle:
```c
static char pool[512];   /* static | RTC_NOINIT (esp32) | heap */
blackbox_config_t cfg = { .pool=pool, .pool_sz=sizeof pool, .flush=BLACKBOX_FLUSH_BATCH_TIME,
                          .flush_ms=60000, .persist_arg="diag.csv", .enabled=false };
blackbox_handle_t h;
blackbox_init(&h, &cfg);
blackbox_enable(&h, true);                       /* default is DISABLED — opt in explicitly */
blackbox_insert(&h, &bb_lc, &(lc_event_t){ .reason=0, .bootcount=17 });
blackbox_tick(&h, dt_ms);                        /* drive time-based flush; also accrues uptime */
blackbox_flush(&h);                              /* persist ("save")                              */
size_t cur=0; char line[BLACKBOX_LINE_MAX];
while (blackbox_pull(&h, &cur, line, sizeof line) > 0) { /* … raw CSV line … */ }

blackbox_status_t st; blackbox_status(&h, &st);
char s[192]; blackbox_status_str(&st, BLACKBOX_STATUS_ALL, s, sizeof s);   /* → "bb: enabled=1 …" */
```

## csv2json
Records are stored keyless; annotate each record struct so the CSV `tag` maps to it:
```c
// @blackbox tag=LC
typedef struct { uint8_t reason; uint16_t bootcount; } lc_event_t;
```
```
csv2json --definitions=records.h < records.csv > records.json    # CLI
```
```js
const { csvToJson, parseDefinitions } = require('./js/csv2json'); // module
```
The struct's field names become the JSON keys (numeric C types → numbers, else strings). Parsed
dynamically per run — no codegen.

## Tag filtering
Tags are capped at `BLACKBOX_TAG_MAX` chars (default **8** → `uint64`; set to **4** → `uint32`) and
packed into that integer, so include/exclude matching on insert is a single integer compare:
```c
blackbox_filter_mode(&h, BLACKBOX_FILTER_EXCLUDE);   /* _OFF (default) | _INCLUDE | _EXCLUDE */
blackbox_filter_add(&h, "MSH");                       /* exclude: skip MSH records            */
```
`INCLUDE` records only listed tags; `EXCLUDE` records all but listed. The list holds up to
`BLACKBOX_FILTER_MAX` (16) tags; status reports the mode, list size, and a `filtered` count.

## Bounding (size / count)
Bound the store at **runtime** — initial value from the config, changeable any time via
`blackbox_bound` — on both backends:
```c
blackbox_bound(&h, 1000, 0);      /* keep ≤ 1000 records (NONE evicts the oldest)        */
blackbox_bound(&h, 0, 65536);     /* cap the file at 64 KB — rotates active → <path>.old  */
```
`NONE` evicts the oldest record when over `max_records`/`max_bytes`; `FILE` rotates the active file to
`<path>.old` (one backup — a rename, not a rewrite) when it would exceed `max_bytes`. `0` on an axis =
unbounded. On a flash filesystem this matters twice over: **batch** (don't write-through) to cut erase
cycles, and **bound** to keep the footprint from filling the partition.

## Backends
- **NONE** — the RAM pool *is* the store (a ring; oldest evicted when full). On esp32 the pool can be
  an `RTC_NOINIT` buffer, so records survive deep sleep; dumped over USB. No flash wear.
- **FILE** — pool stages, `flush` appends to a file (host).
- **MDS_FLASH** — circular append-log in a dedicated esp32 flash partition. *Planned (P3).*

## License
CC BY-NC-SA 4.0 (see [LICENSE](LICENSE)).
