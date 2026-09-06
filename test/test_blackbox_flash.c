/* test_blackbox_flash — exercises the ESP_FLASH backend on the host against a RAM-backed mock of
 * esp_partition (test/mock/esp_partition.h). Unity build: the implementation lives in THIS TU so it
 * shares the mock's single static partition buffer. Validates round-trip, circular wrap + eviction,
 * and boot recovery. Build via `make test-flash`.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0 */
#define MOCK_PART_SECTORS 4                 /* 4 × 4 KB — small, so wrap/eviction hit within the test */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

int test_clock(char *o, size_t n);         /* BLACKBOX_CLOCK hook (named via -D) */
#include "blackbox.h"                       /* BLACKBOX_PERSIST=ESP_FLASH + IMPLEMENTATION via -D; pulls in the mock */

int test_clock(char *o, size_t n) { static unsigned seq = 1000; return snprintf(o, n, "%u:%u", seq++, 7u); }

typedef struct { uint8_t reason; uint16_t idx; } evt_t;
static int enc_evt(const blackbox_struct_config_t *sc, const void *d, char *o, size_t n) {
    (void)sc; const evt_t *e = (const evt_t *)d; return snprintf(o, n, "%u,%u", e->reason, e->idx);
}
static const blackbox_struct_config_t cfg_evt = { "LC", enc_evt, NULL };

/* line = "LC,<clock>,<reason>,<idx>" — idx is the field after the 3rd comma (clock holds no comma). */
static int line_idx(const char *line) {
    int c = 0;
    for (const char *p = line; *p; p++)
        if (*p == ',' && ++c == 3) return atoi(p + 1);
    return -1;
}

int main(void) {
    mock_part_reset();                                     /* virgin flash */
    static char pool[2048];
    blackbox_config_t cfg = {
        .pool = pool, .pool_sz = sizeof pool, .flush = BLACKBOX_FLUSH_MANUAL,
        .persist_arg = "diag", .enabled = true,
    };
    blackbox_handle_t h;
    assert(blackbox_init(&h, &cfg) == 0);
    blackbox_clear(&h);
    blackbox_status_t st; blackbox_status(&h, &st);
    assert(st.count == 0);

    /* round-trip: insert, flush to flash, pull back oldest→newest in order */
    for (int i = 0; i < 10; i++) { evt_t e = { (uint8_t)i, (uint16_t)i }; assert(blackbox_insert(&h, &cfg_evt, &e) == 0); }
    assert(blackbox_flush(&h) == 0);
    blackbox_status(&h, &st);
    assert(st.count == 10);
    { size_t cur = 0; char line[BLACKBOX_LINE_MAX]; int k = 0;
      while (blackbox_pull(&h, &cur, line, sizeof line) > 0) { assert(line_idx(line) == k); k++; }
      assert(k == 10); }
    printf("  esp-flash round-trip ok (10 records)\n");

    /* circular wrap + eviction: push far past capacity; expect a bounded, most-recent window */
    blackbox_clear(&h);
    const int N = 2000;
    for (int i = 0; i < N; i++) {
        evt_t e = { 7, (uint16_t)i };
        assert(blackbox_insert(&h, &cfg_evt, &e) == 0);
        if ((i % 50) == 0) assert(blackbox_flush(&h) == 0);
    }
    assert(blackbox_flush(&h) == 0);
    blackbox_status(&h, &st);
    assert(st.count > 0 && st.count < (uint32_t)N);        /* older records evicted → bounded */
    uint32_t kept = st.count;
    { size_t cur = 0; char line[BLACKBOX_LINE_MAX]; int prev = -1, k = 0, first = -1, last = -1;
      while (blackbox_pull(&h, &cur, line, sizeof line) > 0) {
          int ix = line_idx(line);
          if (first < 0) first = ix;
          if (prev >= 0) assert(ix == prev + 1);           /* strictly contiguous (FIFO by append) */
          prev = ix; last = ix; k++;
      }
      assert(k == (int)kept);
      assert(last == N - 1);                                /* the newest record survives */
      assert(first == N - (int)kept);                       /* survivors are the most-recent window */
      printf("  esp-flash wrap ok (kept %d of %d, idx %d..%d)\n", k, N, first, last);
    }

    /* reboot: flush, drop the RAM pool, re-init over the same (persistent) partition → recover */
    blackbox_deinit(&h);
    memset(pool, 0, sizeof pool);                           /* simulate RAM loss across a reboot */
    blackbox_handle_t h2;
    assert(blackbox_init(&h2, &cfg) == 0);                  /* recovery scan of the flash log */
    blackbox_status(&h2, &st);
    assert(st.count == kept);                               /* recovered count matches the in-session window */
    { size_t cur = 0; char line[BLACKBOX_LINE_MAX]; int k = 0, last = -1;
      while (blackbox_pull(&h2, &cur, line, sizeof line) > 0) { last = line_idx(line); k++; }
      assert(k == (int)kept && last == N - 1); }
    printf("  esp-flash recovery ok (recovered %u records after reboot)\n", kept);

    blackbox_clear(&h2);
    blackbox_status(&h2, &st);
    assert(st.count == 0 && st.corruptions == 0);
    blackbox_deinit(&h2);
    printf("  OK\n");
    return 0;
}
