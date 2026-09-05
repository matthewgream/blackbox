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
