/* example blackbox record definitions (for csv2json) */

// @blackbox tag=LC
typedef struct { uint8_t reason; uint16_t bootcount; } lc_event_t;

// @blackbox tag=MSH
typedef struct { uint32_t rx; uint32_t tx; uint32_t drop; int16_t rssi; } msh_t;
