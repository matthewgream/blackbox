/* mock esp_partition.h — a RAM-backed flash partition with NOR semantics, so the ESP_FLASH backend
 * can be compiled and exercised on the host. Erase fills 0xFF; write AND-masks bits (like real NOR),
 * so an accidental rewrite of an unerased cell corrupts — which the tests would then catch.
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0 */
#ifndef MOCK_ESP_PARTITION_H
#define MOCK_ESP_PARTITION_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0

typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_DATA 1

#ifndef MOCK_PART_SECTORS
#define MOCK_PART_SECTORS 16u          /* 16 × 4 KB = 64 KB — small so wrap/eviction hit quickly */
#endif
#define MOCK_PART_SIZE (MOCK_PART_SECTORS * 4096u)

typedef struct { size_t size; uint8_t *data; } esp_partition_t;

/* One static partition backed by a static buffer. mock_part_reset() re-erases it (a "reflash"). */
static uint8_t  mock_part_buf[MOCK_PART_SIZE];
static esp_partition_t mock_part = { MOCK_PART_SIZE, mock_part_buf };
static int      mock_part_inited = 0;

static void mock_part_reset(void) { memset(mock_part_buf, 0xFF, sizeof mock_part_buf); mock_part_inited = 1; }

static const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char *label) {
    (void)t; (void)s; (void)label;
    if (!mock_part_inited) mock_part_reset();     /* virgin flash on first use */
    return &mock_part;
}

static esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n) {
    if (off + n > p->size) return -1;
    memcpy(dst, p->data + off, n);
    return ESP_OK;
}
static esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n) {
    if (off + n > p->size) return -1;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) p->data[off + i] = (uint8_t)(p->data[off + i] & s[i]);   /* NOR: writes only clear bits */
    return ESP_OK;
}
static esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n) {
    if (off + n > p->size || (off % 4096u) || (n % 4096u)) return -1;   /* must be sector-aligned */
    memset(p->data + off, 0xFF, n);
    return ESP_OK;
}

#endif /* MOCK_ESP_PARTITION_H */
