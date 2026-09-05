/* test_blackbox — exercises the API against whichever PERSIST backend it is compiled with.
 * Build via the Makefile (`make test`), which runs it once for PERSIST_NONE and once for PERSIST_FILE.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

int test_clock(char *o, size_t n);   /* the BLACKBOX_CLOCK hook (defined below, named via -D) */
#include "blackbox.h"

int test_clock(char *o, size_t n) { static unsigned seq = 1000; return snprintf(o, n, "%u:%u", seq++, 7u); }

typedef struct { uint8_t reason; uint16_t bootcount; } evt_t;
static int enc_evt(const blackbox_struct_config_t *sc, const void *d, char *o, size_t n) {
    (void)sc; const evt_t *e = (const evt_t *)d; return snprintf(o, n, "%u,%u", e->reason, e->bootcount);
}
static const blackbox_struct_config_t cfg_evt = { "LC", enc_evt, NULL };

int main(void) {
    static char pool[512];
    blackbox_config_t cfg = {
        .pool = pool, .pool_sz = sizeof pool, .flush = BLACKBOX_FLUSH_MANUAL,
        .persist_arg = "/tmp/bb_test.csv", .enabled = true,
    };
    blackbox_handle_t h;
    assert(blackbox_init(&h, &cfg) == 0);
    blackbox_clear(&h);

    for (int i = 0; i < 5; i++) {
        evt_t e = { (uint8_t)i, (uint16_t)(100 + i) };
        assert(blackbox_insert(&h, &cfg_evt, &e) == 0);
    }
    assert(blackbox_flush(&h) == 0);

    blackbox_status_t st;
    blackbox_status(&h, &st);
    char sbuf[192];
    blackbox_status_str(&st, BLACKBOX_STATUS_ALL, sbuf, sizeof sbuf);
    printf("  %s\n", sbuf);
    assert(st.count == 5 && st.inserted == 5);

    size_t cur = 0; char line[BLACKBOX_LINE_MAX]; int nrec = 0;
    while (blackbox_pull(&h, &cur, line, sizeof line) > 0) { printf("    %s\n", line); nrec++; }
    assert(nrec == 5);

    /* tag filter (integer-packed matching) */
    evt_t ef = { 7, 7 };
    blackbox_clear(&h);
    blackbox_filter_mode(&h, BLACKBOX_FILTER_EXCLUDE);
    assert(blackbox_filter_add(&h, "LC") == 0);
    (void)blackbox_insert(&h, &cfg_evt, &ef);              /* LC excluded → filtered */
    blackbox_status(&h, &st);
    assert(st.count == 0 && st.filtered >= 1 && st.filter_mode == BLACKBOX_FILTER_EXCLUDE && st.filter_count == 1);
    blackbox_filter_clear(&h);
    blackbox_filter_mode(&h, BLACKBOX_FILTER_INCLUDE);
    blackbox_filter_add(&h, "XX");
    (void)blackbox_insert(&h, &cfg_evt, &ef);              /* LC not in include list → filtered */
    blackbox_status(&h, &st);
    assert(st.count == 0);
    blackbox_filter_add(&h, "LC");
    (void)blackbox_insert(&h, &cfg_evt, &ef);              /* LC now included → recorded */
    blackbox_status(&h, &st);
    assert(st.count == 1);
    printf("  filter ok (filtered=%u, tag_bytes=%zu)\n", st.filtered, sizeof(blackbox_tag_t));
    blackbox_filter_clear(&h);
    blackbox_clear(&h);

    /* runtime bound: NONE evicts to max_records; FILE rotates the file by max_bytes */
    blackbox_bound(&h, 2, 0);
    for (int i = 0; i < 4; i++) { evt_t eb = { (uint8_t)i, 0 }; (void)blackbox_insert(&h, &cfg_evt, &eb); }
    blackbox_status(&h, &st);
    assert(st.max_records == 2);
#if BLACKBOX_PERSIST != 1                                 /* NONE ring */
    assert(st.count == 2);                                /* only the 2 newest kept */
    printf("  bound ok (NONE kept %u of 4)\n", st.count);
#else                                                     /* FILE — byte-bound rotation */
    blackbox_clear(&h);
    remove("/tmp/bb_test.csv.old");
    blackbox_bound(&h, 0, 100);                           /* default rotate = BACKUP */
    for (int i = 0; i < 30; i++) { evt_t eb = { (uint8_t)i, (uint8_t)i }; (void)blackbox_insert(&h, &cfg_evt, &eb); (void)blackbox_flush(&h); }
    { FILE *of = fopen("/tmp/bb_test.csv.old", "r"); assert(of != NULL); (void)fclose(of); }  /* backup exists */
    printf("  bound ok (FILE backup: rotated to .old)\n");
    /* overwrite mode: no backup kept */
    remove("/tmp/bb_test.csv.old");
    blackbox_config_t ocfg = cfg; ocfg.rotate = BLACKBOX_ROTATE_OVERWRITE; ocfg.max_bytes = 100;
    blackbox_handle_t oh; assert(blackbox_init(&oh, &ocfg) == 0);
    blackbox_clear(&oh);
    for (int i = 0; i < 30; i++) { evt_t eb = { (uint8_t)i, (uint8_t)i }; (void)blackbox_insert(&oh, &cfg_evt, &eb); (void)blackbox_flush(&oh); }
    { FILE *nf = fopen("/tmp/bb_test.csv.old", "r"); assert(nf == NULL); }   /* overwrite → no backup */
    printf("  bound ok (FILE overwrite: no .old kept)\n");
    blackbox_deinit(&oh);
#endif
    blackbox_bound(&h, 0, 0);
    blackbox_clear(&h);

    blackbox_enable(&h, false);
    evt_t e2 = { 9, 999 };
    (void)blackbox_insert(&h, &cfg_evt, &e2);
    blackbox_status(&h, &st);
    assert(st.dropped >= 1);
    printf("  disabled-drop ok (dropped=%u)\n", st.dropped);

    blackbox_clear(&h);
    blackbox_status(&h, &st);
    assert(st.count == 0);

    /* default (enabled=false) must record nothing */
    blackbox_config_t cfg2 = cfg; cfg2.enabled = false;
    blackbox_handle_t h2;
    assert(blackbox_init(&h2, &cfg2) == 0);
    blackbox_clear(&h2);
    evt_t e3 = { 1, 2 };
    (void)blackbox_insert(&h2, &cfg_evt, &e3);
    blackbox_status_t st2;
    blackbox_status(&h2, &st2);
    assert(st2.count == 0 && st2.dropped >= 1);
    printf("  default-disabled ok\n");
    blackbox_deinit(&h2);

    blackbox_deinit(&h);
    printf("  OK\n");
    return 0;
}
