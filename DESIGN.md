# blackbox — structured diagnostic recorder (design)

A **standalone, generic** structured-record recorder for embedded + host. Public repo
`iotdata-depend/blackbox`, with **no iotdata knowledge**. A consuming project defines its own record
structs + encoders and plugs in backends at **compile time** (the `iotdata_config.h` pattern applied
to a dependency), so unused backend code never links. The library itself is **struct-agnostic** — it
stores **tagged CSV lines**, stamping a generic `clock` column; records can be flushed, pulled,
cleared, and expired.

Repo scope: the **C recorder** + **`csv2json`** (a JS module + thin CLI that turns stored CSV into
JSON by reading the project's record header). **Delivery is out of scope for now** (MQTT bridges,
collectors, viewers — parked, §11). Storage is compact CSV; JSON appears only at a read boundary,
off-device.

*iotdata is one consumer.* Its adapter `iotdata-common/include/iotdata_blackbox.h` provides only the
**backend + clock commonality** (esp32 → MDS-flash partition, linux → file; the iotdata clock
convention) and `#include`s blackbox. It does **not** define records — each project (relay, gateway,
sensor, sds, tsa) defines its own, because what each records differs. The journal **complements**
`m_datastore` (the NVS key/value **counter** store) — a project can record a counter snapshot.

Status: **design, no code written yet.** Repo name `blackbox`; macro prefix `BLACKBOX_`.

---

## 1. Architecture

```
  project:  blackbox_insert(&h, &blackbox_struct_config_event_boot, &boot_info);
              │  descriptor carries the encoder → ONE indirect call, no vtable, no record enum
  LIBRARY (generic, iotdata-depend/blackbox — struct-agnostic, stores tagged CSV)
    • encode   data → CSV payload   (via the descriptor's encode fn)
    • stamp    → "tag,clock,payload\n"   (clock via the config's clock hook)
    • pool     → RAM pool            ┐ tier (1): static | dynamic | extern (RTC_NOINIT)
    • flush    pool lines → persist.write   (plain CSV lines by default; compress only if enabled)
                                     ┘ tier (2): file | mds-flash | custom | none
    • pull/clear/expire/status/enable   (pull returns RAW CSV lines — no struct decode in C)
```

- **Compile-time** decides *which* backend/compressor code links (`#define`s, §5) — footprint.
- **Runtime** `blackbox_init(&h,&config)` passes the instance params + policy; a **handle** holds
  state (so a device could run more than one journal).
- The persist tier moves **opaque chunks** by cursor — it never knows CSV or compression; that
  framing is the library's, keeping every backend trivial.

---

## 2. Repo layout & integration

```
iotdata-depend/blackbox/                    PUBLIC · generic · zero iotdata knowledge
  include/ src/    the C recorder + bundled backends:
                     pool:    static | dynamic | extern
                     persist: none | file(POSIX) | mds-flash(esp-idf, compile-gated) | custom
                     compress:none | heatshrink | custom
  js/csv2json      JS module (+ thin CLI): reads a record header → converts CSV records to JSON
  (delivery — mqtt bridges, collectors, viewers — deliberately OUT for now)
        ▲  #include + compile-time config
iotdata-common/include/iotdata_blackbox.h   iotdata ADAPTER = backends + sizes + clock + 1 common record
  • #if ESP_PLATFORM → persist = MDS-flash("diag"),  pool = RTC RAM (extern);   default SIZES here
    #elif __linux__  → persist = FILE,                pool = heap RAM;            default SIZES here
    (all guarded by #ifndef → a project can override a backend/size before including)
  • supplies the iotdata clock convention (esp32 seq:up_ms / linux epoch ms)
  • defines the ONE shared record — a LIFECYCLE event (boot/start/stop/sleep/wake/reset): struct +
    enc/dec + blackbox_struct_config_lifecycle — every device gets it for free
  • #include "blackbox.h"
        ▲  used by (each adds its OWN domain records; may override a backend/size)
iotdata-example/*  ·  iotdata-device/*  ·  (later) iotdata-machine/*
```

The adapter fixes the platform **backends + pool + sizes + clock**, and the one universal
**lifecycle** record; each project adds its **domain-specific records** on top.

---

## 3. Part 1 — records (project-defined: struct + encoder + descriptor)

The library is handed a small **descriptor** per record type at insert time:

```c
typedef struct blackbox_struct_config {
    const char *tag;                                     /* "BOOT", "MSH", …                          */
    int (*encode)(const struct blackbox_struct_config *sc, const void *data, char *out, size_t outlen);
    int (*decode)(const struct blackbox_struct_config *sc, const char *in, size_t inlen, void *data);
} blackbox_struct_config_t;
```

Per record type a project defines the data struct, an encode/decode pair, and one const descriptor:

```c
typedef struct { uint8_t reason; uint16_t bootcount; } event_boot_t;
static int enc_event(const blackbox_struct_config_t *sc, const void *d, char *o, size_t n) {
    const event_boot_t *e = d;  return snprintf(o, n, "%u,%u", e->reason, e->bootcount);
}
static int dec_event(const blackbox_struct_config_t *sc, const char *in, size_t n, void *d); /* CSV → struct */
const blackbox_struct_config_t blackbox_struct_config_event = { "BOOT", enc_event, dec_event };
```

`blackbox_insert` calls `sc->encode(sc, data, outbuf, outlen)` — the descriptor is handed to its own
encoder as context — then the library stamps `tag,clock` and pools the line. One indirect call per
insert; **no vtable, no record enum, no struct type known to the lib.** The `decode` half is there for
symmetry / any C-side read; the primary read path (JSON) is external `csv2json`. *(An optional
field-list X-macro can generate struct+encode+decode+descriptor from one list; sugar, not required.)*

**Convention:** the encoder emits columns in **struct-declaration order**, because `csv2json` maps
CSV columns onto struct fields positionally (below).

### CSV wire format
`tag,clock,<payload columns…>\n` — positional, keyless, compact. iotdata example records:

| tag | payload | emitted by |
|-----|---------|------------|
| `RST` | `reason,bootcount` | all (boot) |
| `PWR` | `event` | sensors, relay |
| `MSH` | `rx,tx,drop,fwd,ack,rssi` | relay, gateway |
| `BAT` | `mv,pct` | sensors |
| `ERR` | `code,detail` | all |
| `CNT` | `k=v;k=v;…` (m_datastore snapshot) | all |

```
RST,1041:52,8,17                          # esp32 clock = seq:up_ms
MSH,1725539696789,913,44,7,806,801,-83    # gateway clock = epoch ms, same column
```

### CSV→JSON — `csv2json` (JS, in the blackbox repo)
A **JS module (+ CLI)** reads the project's **record header** and converts stored CSV **dynamically**
(parsing one small header per run is plenty fast — no codegen):

```
csv2json --definitions=/path/records.h  < records.csv  > records.json
```

It parses the record structs → `tag → [field name, type]`, then maps each CSV line's positional
columns onto those names — the **struct field name is the JSON key**, numbers bare, `str` quoted.
Used wherever records leave CSV for JSON: the **gateway answering a request for records**, or a
**bench host decoding a USB export**. One header, two consumers (the C encoder + `csv2json`) that
can't drift. Being a module, the JS master / a web UI can `import` it instead of shelling out.

---

## 4. Part 2 — lifecycle API (handle-based)

```c
blackbox_handle_t h;
blackbox_init  (&h, &config);                              /* §5 */

blackbox_insert(&h, &blackbox_struct_config_event, &boot); /* sc->encode(sc,data,…), stamp tag,clock → pool */
blackbox_tick  (&h);                    /* drive time-based flush / expiry — call periodically from the loop */
blackbox_flush (&h);                    /* pool → persist   ("save")                 */
blackbox_pull  (&h, &cursor, line, n);  /* read stored CSV lines  ("load")           */
blackbox_clear (&h);
blackbox_expire(&h);
blackbox_status(&h, &stats);            /* count, bytes, used%, dropped, enabled     */
blackbox_enable(&h, on);                /* runtime gate (MANAGE)                     */
blackbox_deinit(&h);
```

- **insert ≠ flush.** `insert` stages into the RAM pool (cheap, frequent); `flush` persists a chunk
  (rare, batched) — the flash-wear lever. Flush triggers (config): pool-full · timed · pre-deep-sleep
  · manual. Sensors flush pre-sleep + pool-full; relay timed/size.
- **Expire has no wall clock on the ESP32s** → on-device it is **count/size** based (evict lowest
  `seq` when the log laps — implicit on the MDS append-log, §6). Age-based expiry only on the host
  (file rotation by mtime). Same call; the backend decides.

---

## 5. Part 3 — configuration: compile-time by default, runtime where it must be

**As much as possible is compile-time** (`#define`s in the adapter / project build — unused code
never links): the backend, the compressor, the clock, the pool kind, the expire policy. The runtime
`blackbox_config_t` carries only what genuinely varies at run time.

```c
/* compile-time — unused code never links */
#define BLACKBOX_PERSIST   BLACKBOX_PERSIST_MDS_FLASH   /* | _FILE | _CUSTOM | _NONE  */
#define BLACKBOX_MDS_PART  "diag"
#define BLACKBOX_POOL      BLACKBOX_POOL_EXTERN          /* | _STATIC(n) | _DYNAMIC    */
#define BLACKBOX_CLOCK     iotdata_bb_clock              /* clock hook (§7)            */
#define BLACKBOX_EXPIRE    BLACKBOX_EXPIRE_COUNT         /* | _BYTES | _AGE            */
#define BLACKBOX_COMPRESS  BLACKBOX_COMPRESS_NONE        /* dormant hook — see §6 note */

/* runtime — only what genuinely varies */
typedef struct {
    void    *pool;  size_t pool_sz;   /* the RTC/heap buffer (a runtime address)                */
    blackbox_flush_t flush;           /* BATCH_SIZE | BATCH_TIME(+flush_ms) | PRESLEEP | MANUAL |
                                         WRITE_THROUGH  ← persist every insert, for debugging     */
    uint32_t flush_ms;                /* for BATCH_TIME                                          */
    uint32_t expire_limit;            /* bytes/count for the compile-time EXPIRE policy          */
    bool     enabled;                 /* initial gate; toggled at run time via MANAGE            */
} blackbox_config_t;

blackbox_init(&h, &config);
```

The flush mode is runtime on purpose: **`WRITE_THROUGH`** bypasses the pool and persists every record
immediately — you lose batching (more flash wear) but gain durability **right up to a crash/reset**,
which is exactly what specialised debugging wants. Normal operation batches (`BATCH_*` / `PRESLEEP`).
`enabled` / `clear` / `expire` are the other runtime knobs (MANAGE, §8).

---

## 6. Backends

**Tier 1 — RAM pool** (where `insert` stages; the only store in `PERSIST_NONE`): `STATIC n` · `DYNAMIC`
· `EXTERN ptr` (project RTC_NOINIT buffer → survives deep sleep, accumulates across wakes).

**Tier 2 — persistent** (one, compile-selected):
- **`FILE`** (host) — `fopen("a")`/`fwrite`/`fgets`; expire = rotate/delete by size or **mtime**.
- **`MDS_FLASH`** (esp32, bundled, gated) — circular append-log in a **dedicated `diag` partition**
  (`type=data, subtype=0x40`), record = a length-framed CSV line (`[u16 len][payload]`; a `flags` byte
  is added only when `BLACKBOX_COMPRESS` is on, marking a compressed block). Append forward; crossing a
  4 KB sector erases the next sector first → **evicts oldest → expiry-by-size is free.** Write cursor
  in RTC RAM + occasional flash checkpoint; recovered by scan on boot if stale. Batched appends + one
  erase per lap ⇒ negligible wear. It is the **log mode** of the MDS family, in its **own partition,
  decoupled from the NVS counter store** — two partitions, two failure domains. Default (uncompressed)
  a raw partition dump is **human-readable CSV**.
- **`CUSTOM`** — project supplies the ops. **`NONE`** — RAM-pool-only.

### Compression? — deferred (default off)
Probably unnecessary: the CSV is already compact, the partition roomy (thousands of records), and
plain lines keep the store **readable** (`cat` a flash dump / `tail` the gateway file) — worth a lot
in a debugging tool. So the default is line-oriented + uncompressed. `BLACKBOX_COMPRESS` stays a
dormant compile-time hook (heatshrink / custom) for a future high-volume need; turning it on switches
storage to compressed blocks (opaque, decompress-on-read). Not in P1.

---

## 7. Clock & ordering — one generic field

One `clock` column, set by the config's `clock` hook:
- **host:** real wall-clock (epoch ms) — NTP.
- **ESP32:** synthetic `seq:up_ms` — `seq` = global monotonic in RTC RAM (survives sleep,
  checkpointed) → total order; `up_ms` = `esp_timer` ms this boot. Colon-joined = one column, sort by
  the `seq` part.

No wall clock stored on the ESP32s; the gateway maps synthetic → real on ingest, or keeps it.

---

## 8. Remote control — MANAGE `DIAG_*` (0x20 group)

Into the existing `IOTDATA_MESH_MANAGE_CMD_*` enum / `relay_manage` switch (0x01–0x13 used):

| cmd | name | args | response |
|-----|------|------|----------|
| `0x20` | `DIAG_STATUS_REQUEST` | — | count, bytes, used%, enabled |
| `0x21` | `DIAG_ENABLE` | `on(1)` | ack |
| `0x22` | `DIAG_CLEAR` | — | ack |
| `0x23` | `DIAG_EXPIRE` | — | ack |
| `0x24` | `DIAG_READ_REQUEST` | `count(1)` | recent-N CSV lines, chunked to the mesh MTU (gateway can `csv2json` them) |

Bulk retrieval stays on **USB-JTAG** (`DIAG DUMP`); the 2400 bps mesh carries only status + recent-N
+ control.

---

## 9. Per-device configuration

| device | pool (tier 1) | persist (tier 2) | flush | compress | expire |
|--------|---------------|------------------|-------|----------|--------|
| **gateway** (linux) | heap | FILE + rotate | TIME | gzip-on-rotate | AGE/size |
| **relay** (esp32, always-on) | small EXTERN(RTC) | MDS_FLASH | SIZE/TIME | on | COUNT/size |
| **sensor** (esp32, deep-sleep) | EXTERN(RTC), accumulates | MDS_FLASH | PRESLEEP + pool-full | on | COUNT/size |
| **simulator** | heap | NONE / stdout | TIME | off | — |

Same API + descriptors everywhere; only `config` + the `#define`s differ.

---

## 10. Rollout & build plan

**Pieces & responsibilities**
1. **`iotdata-depend/blackbox`** (public) — the C recorder + the MDS esp32 backend + `csv2json`.
2. **`iotdata-common/include/iotdata_blackbox.h`** — platform backends + pool + **sizes** (esp32:
   MDS-flash + RTC pool; linux: file + heap pool) + the iotdata clock, and the one common
   **lifecycle** record (start/stop/…).
3. **Each project** — a header/section that: `#include`s the adapter; declares its **domain record
   structs + encoders/decoders + descriptors**; holds **init/term** (`blackbox_init` with the
   project's `config`, `blackbox_deinit`); and on **esp32** adds the **`diag` partition** to its
   partition map.
4. **Gateway** — an MQTT **command bridge** (pub/sub) for diagnostics, carrying the **CSV** format
   as-is; CSV→JSON stays elsewhere for now.
5. **Relay (+ others)** — extend the MANAGE command set with **DIAG enable / disable / clear /
   dump-to-USB** (§8).

Then: add more record types per device — the shared **lifecycle** record is the first.

**Phases**
- **P1 — prove the seam.** library (`init/insert/tick/flush/pull` + `POOL_STATIC` + `FILE` backend +
  `MDS_FLASH` RAM-stub) + `iotdata_blackbox.h` (with the lifecycle record). Gateway records lifecycle
  + a stat to a CSV file; relay records lifecycle + a stat. `csv2json` turns the file into JSON.
- **P2 — MANAGE + gateway bridge.** `DIAG_ENABLE/DISABLE/CLEAR/DUMP` (0x20) on the relay; gateway CSV
  command bridge to MQTT.
- **P3 — real flash + retrieval.** flesh out `MDS_FLASH` (partition, wraparound, recovery) +
  `DIAG_READ` recent-N over mesh + USB `DIAG DUMP`. (compression still deferred.)
- **P4 — later / out of scope now.** delivery upstream; collectors/viewers; CSV→JSON in the flow.

---

## 11. Open / later
- macro prefix `BLACKBOX_` vs terser `BBOX_`; descriptor name `blackbox_struct_config_t`.
- heatshrink vs. hand-rolled RLE for the ESP32 compressor.
- whether an optional field-list X-macro is worth shipping to generate struct+encoder+descriptor.
- delivery (mqtt/collect/view) — parked.
- ties into the future MANAGE **config** chunk and OTA — both want this journal for field debugging.

## 12. P1 implementation notes (this repo)
- **Backends implemented:** `NONE` (pool-as-ring; on esp32 an `RTC_NOINIT` pool → survives deep
  sleep) and `FILE` (host). `MDS_FLASH` and `CUSTOM` `#error` for now (P3) so a misconfig fails loud.
- **Default is DISABLED** — `blackbox_enable(h, true)` to opt in. (Per-record-type enable/disable is a
  planned filter — the `filters` field in the status is the placeholder.)
- **Status** (`blackbox_status_t` + `blackbox_status_str(st, flags, buf, n)`): enabled, persist,
  filters, count/dropped/inserted/flushes, bytes/used%/pool_sz, uptime. Renders selectively via the
  `BLACKBOX_STATUS_*` section flags. This is what the gateway's MQTT status query returns.
- **csv2json tag→struct mapping:** an `// @blackbox tag=LC` annotation above each record struct; the
  struct field names become the JSON keys. Fields must be declared in CSV-column (encoder) order.
- **`blackbox_tick(h, dt_ms)`** drives `BATCH_TIME` flush and accrues `uptime_ms`.
