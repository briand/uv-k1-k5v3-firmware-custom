/* Host tests for decm20: craft a CRC-valid M20 frame, check parse fields,
 * then softin the rawheader + body through decm20_push (2 ADC samples/chip).
 * Needs only a C compiler. Run via test/run_tests.sh. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../sonde_info.h"
#include "../decm20.h"
#include "../decm20.c"

static uint16_t mk_check(const uint8_t *msg, uint8_t len)
{
    uint16_t c = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t b = msg[i];
        b = (uint8_t)((b >> 1) | (b << 7));
        b ^= (uint8_t)((b >> 2) & 0xFFu);
        uint8_t t6 = (uint8_t)((c ^ (c >> 2) ^ (c >> 4)) & 1u);
        uint8_t t7 = (uint8_t)(((c >> 1) ^ (c >> 3) ^ (c >> 5)) & 1u);
        uint8_t t = (uint8_t)((c & 0x3Fu) | (t6 << 6) | (t7 << 7));
        uint8_t s = (uint8_t)((c >> 7) & 0xFFu);
        s ^= (uint8_t)((s >> 2) & 0xFFu);
        c = (uint16_t)(((c & 0xFFu) << 8) | (uint8_t)(b ^ t ^ s));
    }
    return c;
}

static void put_be32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void put_be24(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* One Manchester chip: two ADC samples at 19.2 kHz. */
static void feed_chip(decm20_t *d, uint8_t chip)
{
    uint16_t s = chip ? 3000u : 1000u;
    (void)decm20_push(d, s);
    (void)decm20_push(d, s);
}

static void feed_bit(decm20_t *d, uint8_t bit, uint8_t *prev)
{
    uint8_t man = bit ? *prev : (uint8_t)(1u ^ *prev);
    if (man) { feed_chip(d, 0); feed_chip(d, 1); }
    else     { feed_chip(d, 1); feed_chip(d, 0); }
    *prev = man;
}

/* auto_rx rawheader: 10011001100110010100110010011001 */
static void feed_rawheader(decm20_t *d)
{
    uint32_t h = 0x99994C99u;
    for (int i = 0; i < 32; i++)
        feed_chip(d, (uint8_t)((h >> (31 - i)) & 1u));
}

static void feed_frame_body(decm20_t *d, const uint8_t *f)
{
    uint8_t prev = 0;
    uint16_t nb = (uint16_t)((f[0] + 1u) * 8u);
    for (uint16_t bi = 0; bi < nb; bi++) {
        uint8_t bit = (uint8_t)((f[bi >> 3] >> (7 - (bi & 7))) & 1u);
        feed_bit(d, bit, &prev);
    }
}

static void build_frame(uint8_t *f)
{
    memset(f, 0, M20_FRAME_LEN);
    f[0] = 0x45; f[1] = 0x20;
    f[0x04] = 0x09; f[0x05] = 0x05; /* tRaw ≈ mid range */
    put_be24(f + 0x08, 120000);     /* alt 1200 m */
    {
        /* ym=29 → 206, n=1, b=0, sn=1234 → "206-1-01234" */
        uint32_t sn24 = ((uint32_t)1234 << 10) | 29u;
        f[0x12] = (uint8_t)sn24;
        f[0x13] = (uint8_t)(sn24 >> 8);
        f[0x14] = (uint8_t)(sn24 >> 16);
    }
    f[0x15] = 7;
    f[0x16] = 0;                    /* pressure LSB (used if fw>=7) */
    f[0x24] = 0x00; f[0x25] = 0x10; /* → ~16 hPa raw /4096 → check HAS_PRESS */
    put_be32(f + 0x1C, 48800000);   /* 48.8 N */
    put_be32(f + 0x20, 2300000);    /* 2.3 E */
    f[0x43] = 7;                    /* fwVer at flen-2 */
    {
        uint16_t cs = mk_check(f, 0x44);
        f[0x44] = (uint8_t)(cs >> 8);
        f[0x45] = (uint8_t)(cs & 0xFF);
    }
}

int main(void)
{
    uint8_t f[M20_FRAME_LEN];
    int fail = 0;
    build_frame(f);

    /* ---- parse-only ---- */
    {
        decm20_t d;
        sonde_info_t out;
        memset(&d, 0, sizeof d);
        decm20_init(&d);
        memcpy(d.frm, f, M20_FRAME_LEN);
        d.state = 2;
        memset(&out, 0, sizeof out);
        if (!decm20_parse(&d, &out)) {
            fprintf(stderr, "parse: CRC/header rejected\n");
            fail++;
        } else if (strcmp(out.id, "206-1-01234") != 0) {
            fprintf(stderr, "parse: id got %s\n", out.id);
            fail++;
        } else if (!(out.flags & SONDE_HAS_POS) || out.alt_m != 1200) {
            fprintf(stderr, "parse: pos/alt flags=%u alt=%ld\n",
                    out.flags, (long)out.alt_m);
            fail++;
        } else if (out.latS != 175680 || out.lonS != 8280) {
            fprintf(stderr, "parse: lat=%ld lon=%ld\n",
                    (long)out.latS, (long)out.lonS);
            fail++;
        } else
            printf("  ✅ parse fields\n");
    }

    /* ---- softin path ---- */
    {
        decm20_t d;
        sonde_info_t out;
        memset(&d, 0, sizeof d);
        decm20_init(&d);
        /* Square host chips are sample-aligned; start the DPLL on a boundary. */
        d.ph = 0;
        /* A few alternating chips so the DC mean settles before the header. */
        for (int i = 0; i < 16; i++) feed_chip(&d, (uint8_t)(i & 1u));
        feed_rawheader(&d);
        feed_frame_body(&d, f);
        /* Trailing chips flush the last mid-sample DPLL boundary. */
        for (int i = 0; i < 8; i++) feed_chip(&d, (uint8_t)(i & 1u));
        memset(&out, 0, sizeof out);
        if (d.state != 2) {
            fprintf(stderr, "softin: state=%u nbit=%u\n", d.state, d.nbit);
            fail++;
        } else if (!decm20_parse(&d, &out) || !(out.flags & SONDE_HAS_POS)) {
            fprintf(stderr, "softin: parse fail id=%s flags=%u\n",
                    out.id, out.flags);
            fail++;
        } else if (strcmp(out.id, "206-1-01234") != 0 || out.alt_m != 1200) {
            fprintf(stderr, "softin: id=%s alt=%ld\n", out.id, (long)out.alt_m);
            fail++;
        } else
            printf("  ✅ softin frame\n");
    }

    if (fail) {
        fprintf(stderr, "  %d failed\n", fail);
        return 1;
    }
    puts("OK");
    return 0;
}
