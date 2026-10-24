/* Common decoded radiosonde fields (M20 app). */
#ifndef SONDE_INFO_H
#define SONDE_INFO_H
#include <stdint.h>

typedef struct {
    char     id[12];
    int32_t  latS, lonS, alt_m;
    uint16_t tRaw;      /* thermistor ADC incl. range bits (frame 0x04..0x05) */
    int16_t  temp_c10;
    uint16_t press_h10; /* hPa x10 */
    uint8_t  flags;
} sonde_info_t;

enum {
    SONDE_HAS_POS   = 1u,
    SONDE_HAS_ALT   = 2u,
    SONDE_HAS_TEMP  = 4u,
    SONDE_HAS_PRESS = 8u,
    SONDE_HAS_ID    = 32u
};
#endif
