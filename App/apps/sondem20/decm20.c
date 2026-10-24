/* M20 soft-decision decoder (auto_rx m20mod softin).
 *
 * Chip rate 9600 Manchester; ADC at 19.2 kHz → nominally 2 samples/chip.
 * Search: correlate 32-chip rawheader, then softin the next 32 chips to
 * 0x4520/0x4320. Body polarity: an inverted AF chain only flips the first
 * softin bit (prev starts at 0), so inv = MSB of that word.
 *
 * Timing: chip boundaries often fall mid-sample. decm20_push splits the
 * sample at ph (Q8) and pulls ph from interpolated zero crossings.
 * DC mean is frozen in ST_DATA so PA4 bias creep does not spoil late CRC bits.
 */
#include "decm20.h"

#define FS       19200u
#define SPB_Q8   ((int32_t)((FS << 8) / M20_BAUD))
#define MEAN_SH  6
#define CHIP_MSK ((uint8_t)(M20_CHIP_N - 1u))
enum { ST_SRCH = 0, ST_DATA = 1, ST_DONE = 2 };

#define RAW_HDR 0x99994C99u /* "10011001100110010100110010011001" */

static uint16_t checkM10(const uint8_t *msg, uint8_t len)
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

static uint32_t be(const uint8_t *p, uint8_t n)
{
    uint32_t v = 0;
    while (n--) v = (v << 8) | *p++;
    return v;
}

static int32_t u1e6ToArcSec(const uint8_t *p)
{
    int32_t v = (int32_t)be(p, 4);
    int32_t neg = v < 0;
    uint32_t u = (uint32_t)(neg ? -v : v);
    uint32_t as = (u / 1000000u) * 3600u + ((u % 1000000u) * 3600u) / 1000000u;
    return neg ? -(int32_t)as : (int32_t)as;
}

static int16_t softOf(int32_t integ)
{
    int32_t v = integ >> 8;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return (int16_t)v;
}

/* Rawheader soft match, either polarity. Radio AF weak → ~60% ratio. */
static bool hdrOk(const int16_t *ch, uint8_t base)
{
    int32_t s = 0;
    uint32_t e = 0;
    for (uint8_t i = 0; i < 32u; i++) {
        int32_t c = ch[(uint8_t)(base + i) & CHIP_MSK];
        e += (uint32_t)(c >= 0 ? c : -c);
        if ((RAW_HDR >> (31u - i)) & 1u) s += c;
        else s -= c;
    }
    if (s < 0) s = -s;
    return e > 120u && (uint32_t)s * 5u > e * 3u;
}

/* Softin 32 chips → 16-bit word; *prev_out = last Manchester bit.
 * Inverted polarity only flips the first bit (prev starts at 0). */
static uint16_t softWord(const int16_t *ch, uint8_t base, uint8_t *prev_out)
{
    uint16_t w = 0;
    uint8_t prev = 0, i;
    for (i = 0; i < 16u; i++) {
        int32_t s1 = ch[(uint8_t)(base + i * 2u) & CHIP_MSK];
        int32_t s2 = ch[(uint8_t)(base + i * 2u + 1u) & CHIP_MSK];
        uint8_t man = (s2 >= s1) ? 1u : 0u;
        w = (uint16_t)((w << 1) | (prev == man ? 1u : 0u));
        prev = man;
    }
    *prev_out = prev;
    return w;
}

static void putBit(decm20_t *d, uint8_t bit)
{
    if (d->nbit >= d->needBits) return;
    /* Clear each body byte when its first bit arrives instead of pausing at lock. */
    if (!(d->nbit & 7u)) d->frm[d->nbit >> 3] = 0;
    if (bit) d->frm[d->nbit >> 3] |= (uint8_t)(1u << (7u - (d->nbit & 7u)));
    d->nbit++;
}

static void emitChip(decm20_t *d, int16_t soft)
{
    if (d->state == ST_SRCH) {
        uint8_t prev;
        uint16_t w;
        d->chips[d->cpos] = soft;
        d->cpos = (uint8_t)((d->cpos + 1u) & CHIP_MSK);
        if (d->nfill < 64u) { d->nfill++; return; }
        /* oldest 32 = rawheader, newest 32 = first 16 softin bits */
        if (!hdrOk(d->chips, d->cpos)) return;
        w = softWord(d->chips, (uint8_t)(d->cpos + 32u) & CHIP_MSK, &prev);
        d->inv = (uint8_t)(w >> 15);
        w &= 0x7FFFu;
        if (w != 0x4520u && w != 0x4320u) return;
        d->frm[0] = (uint8_t)(w >> 8);
        d->frm[1] = 0x20u;
        d->nbit = 16u;
        d->needBits = (uint16_t)((d->frm[0] + 1u) * 8u);
        d->prev = (uint8_t)(prev ^ d->inv);
        d->state = ST_DATA;
        return;
    }
    if (++d->guard > (uint16_t)(d->needBits * 3u + 64u)) {
        d->state = ST_DONE;
        return;
    }
    {
        int32_t c = d->inv ? -(int32_t)soft : (int32_t)soft;
        uint8_t man;
        if (!d->half) { d->h1 = c; d->half = 1u; return; }
        d->half = 0;
        man = (c >= d->h1) ? 1u : 0u;
        putBit(d, d->prev == man ? 1u : 0u);
        d->prev = man;
        if (d->nbit >= d->needBits) d->state = ST_DONE;
    }
}

void decm20_init(decm20_t *d)
{
    uint8_t *p = (uint8_t *)d;
    for (uint16_t i = 0; i < sizeof *d; i++) p[i] = 0;
    d->meanQ8 = -1;
    d->ph = SPB_Q8 / 2;
}

void decm20_rearm(decm20_t *d)
{
    int32_t mean = d->meanQ8;
    decm20_init(d);
    d->meanQ8 = mean;
}

bool decm20_busy(const decm20_t *d) { return d->state == ST_DATA; }

bool decm20_push(decm20_t *d, uint16_t sample)
{
    int32_t xq, e;
    uint8_t s;

    if (d->state == ST_DONE) return true;

    xq = (int32_t)sample << 8;
    if (d->meanQ8 < 0) d->meanQ8 = xq;
    e = xq - d->meanQ8;
    if (d->state != ST_DATA)
        d->meanQ8 += (e + (1 << (MEAN_SH - 1))) >> MEAN_SH;
    /* ph: Q8 offset to the next chip boundary from this sample's start. */
    s = e > 0 ? 1u : 0u;
    if (s != d->sign) {
        uint32_t a = (uint32_t)(d->pe < 0 ? -d->pe : d->pe);
        uint32_t b = (uint32_t)(e < 0 ? -e : e);
        int32_t err = (int32_t)((a << 8) / (a + b + 1u)) - 128 - d->ph;
        if (err < -(SPB_Q8 / 2)) err += SPB_Q8;
        d->ph += err >> 3;
        d->sign = s;
    }
    d->pe = e;
    if (d->ph < 256) {
        int32_t part = (e * (d->ph < 0 ? 0 : d->ph)) >> 8;
        int16_t chip = softOf(d->integ + part);
        d->integ = e - part;
        d->ph += SPB_Q8 - 256;
        emitChip(d, chip);
        return d->state == ST_DONE;
    }
    d->integ += e;
    d->ph -= 256;
    return false;
}

static char *dig(char *o, uint32_t v, uint32_t div)
{
    for (; div; div /= 10u) *o++ = (char)('0' + (v / div) % 10u);
    return o;
}

bool decm20_parse(decm20_t *d, sonde_info_t *out)
{
    const uint8_t *f = d->frm;
    uint8_t pc = (uint8_t)(f[0] - 1u);
    uint32_t v;

    if (pc >= M20_FRAME_LEN - 1u || be(f + pc, 2) != checkM10(f, pc))
        return false;
    /* SN "YMM-n-bnnnn" as auto_rx get_SN, from LE 24-bit at 0x12 */
    {
        uint32_t sn = f[0x12] | ((uint32_t)f[0x13] << 8) | ((uint32_t)f[0x14] << 16);
        uint8_t ym = (uint8_t)(sn & 0x7Fu);
        char *o = dig(out->id, ym / 12u, 1u);
        o = dig(o, ym % 12u + 1u, 10u); *o++ = '-';
        o = dig(o, ((sn >> 7) & 7u) + 1u, 1u); *o++ = '-';
        o = dig(o, (sn >> 23) & 1u, 1u);
        o = dig(o, (sn >> 10) & 0x1FFFu, 1000u);
        *o = 0;
    }
    out->flags = SONDE_HAS_ID | SONDE_HAS_ALT;
    out->latS = u1e6ToArcSec(f + 0x1C);
    out->lonS = u1e6ToArcSec(f + 0x20);
    if (out->latS || out->lonS) out->flags |= SONDE_HAS_POS;
    v = be(f + 0x08, 3);
    if (v & 0x800000u) v |= 0xFF000000u;
    out->alt_m = (int32_t)v / 100;
    out->tRaw = (uint16_t)(f[0x04] | (f[0x05] << 8));
    /* hPa = (f[0x25] f[0x24] p0) / 4096, p0 = f[0x16] from FW 7 (DF9DQ) */
    v = ((uint32_t)f[0x25] << 16) | ((uint32_t)f[0x24] << 8) |
        (f[pc - 1u] >= 7u ? f[0x16] : 0u);
    if (v && v <= 0xA00000u) {
        out->press_h10 = (uint16_t)((v * 10u) >> 12);
        out->flags |= SONDE_HAS_PRESS;
    }
    return true;
}
