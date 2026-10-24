/* Copyright 2026 — Apache-2.0
 * Sonde M20: decode Meteomodem M20 on the radio (PA4 @ 19.2 kHz, BK4829).
 * GPS first, then alt / T / P when available.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"
#include "sondem20_assets.h"
#include "sonde_info.h"
#include "decm20.c"

static const app_api_t *Gdiv;
uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d){ return Gdiv->uidivmod(n, d); }
uint64_t __aeabi_uidiv(uint32_t n, uint32_t d) __attribute__((alias("__aeabi_uidivmod")));
uint64_t __aeabi_idivmod(int32_t n, int32_t d){ return Gdiv->idivmod(n, d); }
uint64_t __aeabi_idiv(int32_t n, int32_t d) __attribute__((alias("__aeabi_idivmod")));
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *D = d; const uint8_t *S = s;
    while (n--) *D++ = *S++;
    return d;
}

#define SYST_LOAD   (*(volatile uint32_t *)0xE000E014u)
#define SYST_VAL    (*(volatile uint32_t *)0xE000E018u)
#define ADC_SR      (*(volatile uint32_t *)0x40012400u)
#define ADC_CR2     (*(volatile uint32_t *)0x40012408u)
#define ADC_SMPR3   (*(volatile uint32_t *)0x40012414u)
#define ADC_SQR3    (*(volatile uint32_t *)0x40012438u)
#define ADC_DR      (*(volatile uint32_t *)0x40012450u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define DAC_CR      (*(volatile uint32_t *)0x40007400u)
#define DAC_SWTRIGR (*(volatile uint32_t *)0x40007404u)
#define DAC_DHR12R1 (*(volatile uint32_t *)0x40007408u)
#define RCC_APBENR1 (*(volatile uint32_t *)0x4002103Cu)
#define RCC_DACEN   (1u << 29)
#define DAC_CR_BIAS ((1u << 0)|(1u << 1)|(1u << 2)|(7u << 3))
#define BIAS_CODE   2048u
#define ADC_SR_EOC     (1u << 1)
#define ADC_CR2_START  ((1u << 22)|(1u << 20))
#define SMP8_POS 24u
#define SMP4_POS 12u
#define ADC_CH_PA4 4u
#define FS 19200u
#define CYC_PER_SAMPLE (48000000u / FS)
#define REG_2B 0x2B
#define REG_73 0x73
#define REG_30 0x30
#define REG_43 0x43
/* Widest IF filter for the session (restored on exit). */
#define REG_43_M20 0x7E2Cu
#define REG_47 0x47
/* AF select = 9 (bypass). Stock FM AF low-passes ~3–4 kHz and kills 1-chip runs. */
#define REG_47_M20 0x6942u
#define REG_38 0x38
#define REG_39 0x39
#define STEP_10HZ 500
#define OFF_MAX 10
/* Keep normal gaps sparse; poll faster only while F awaits its next key. */
#define KEY_IDLE_EVERY 4800u
#define KEY_ARMED_EVERY 960u
#define ARMED_REFRESH_SLOTS 5u
#define CFG_MAGIC 0x21u
#define SPK_X 59u
#define BK_REG_13 0x13
#define ATT_SETTLE 40

typedef struct { uint8_t spk, att; } app_cfg_t;
typedef struct { uint32_t sqr3, smpr3, moder, dac, rcc, dhr; } saved_hw_t;

static struct {
    const app_api_t *A;
    sonde_info_t info;
    uint32_t vfoFreq;
    uint8_t prevKey, line;
    app_cfg_t cfg;
    int8_t offSteps;
    bool running, fArm;
    int16_t rssi;
    uint16_t nOk;
} g;

static char *put(char *o, const char *s){ while (*s) *o++ = *s++; return o; }
static uint8_t subu(uint32_t *v, uint32_t d){ uint8_t q = 0; while (*v >= d) { *v -= d; q++; } return q; }
static char *puti(char *o, int32_t v)
{
    static const uint16_t P[] = { 10000u, 1000u, 100u, 10u, 1u };
    uint32_t u = (uint32_t)(v < 0 ? (*o++ = '-', -v) : v);
    bool lead = false;
    for (uint8_t i = 0; i < 5u; i++) {
        uint8_t c = subu(&u, P[i]);
        if (c || lead || i == 4u) { *o++ = (char)('0' + c); lead = true; }
    }
    return o;
}
static char *putPos(char *o, int32_t s, char pos, char neg)
{
    uint32_t a = (uint32_t)(s < 0 ? -s : s);
    o = puti(o, subu(&a, 3600u)); *o++ = '.';
    for (uint8_t k = 0; k < 5u; k++) { a *= 10u; *o++ = (char)('0' + subu(&a, 3600u)); }
    *o++ = s < 0 ? neg : pos;
    return o;
}
static char *putFreq(char *o, uint32_t f)
{
    uint16_t mhz = 0; f += 50u;
    while (f >= 100000u) { f -= 100000u; mhz++; }
    o = puti(o, mhz); *o++ = '.';
    *o++ = (char)('0' + subu(&f, 10000u));
    *o++ = (char)('0' + subu(&f, 1000u));
    *o++ = (char)('0' + subu(&f, 100u));
    return o;
}
static char *put1(char *o, int32_t v10)
{
    if (v10 < 0) { *o++ = '-'; v10 = -v10; }
    o = puti(o, v10 / 10); *o++ = '.'; *o++ = (char)('0' + v10 % 10);
    return o;
}
static void tiny(char *buf, uint8_t x, uint8_t y, char *end)
{ *end = '\0'; g.A->print_tiny(buf, x, y, false, true); }

static void tune(void)
{
    uint32_t f = (uint32_t)((int32_t)g.vfoFreq + (int32_t)g.offSteps * STEP_10HZ);
    g.A->bk_write(REG_38, (uint16_t)(f & 0xFFFFu));
    g.A->bk_write(REG_39, (uint16_t)(f >> 16));
    uint16_t r30 = g.A->bk_read(REG_30);
    g.A->bk_write(REG_30, 0); g.A->bk_write(REG_30, r30);
}
static void applyAtt(void)
{
    uint16_t reg = g.A->bk_read(BK_REG_13), gain;
    g.A->asset_read((uint16_t)(ATT_REG13 + g.cfg.att * 2u), &gain, sizeof gain);
    g.A->bk_write(BK_REG_13, (uint16_t)((reg & ~0x03FFu) | gain));
    g.A->set_agc(false);
}
static char *putAttVal(char *o, const char *s)
{
    if (g.cfg.att < ATT_BYP0) {
        uint8_t db;
        g.A->asset_read((uint16_t)(ATT_DB + g.cfg.att), &db, 1);
        o = puti(o, db);
        o = put(o, s + T_DB);
    } else o = put(o, g.cfg.att == ATT_BYP0 ? s + T_BYP : s + T_BYP_PLUS);
    return o;
}
static char *putAtt(char *o, const char *s)
{ return putAttVal(put(o, s + T_ATT), s); }
static void biasOn(const saved_hw_t *s)
{
    GPIOA_MODER = s->moder | (3u << 8);
    RCC_APBENR1 = s->rcc | RCC_DACEN;
    DAC_CR = DAC_CR_BIAS; DAC_DHR12R1 = BIAS_CODE; DAC_SWTRIGR = 1u;
}
static void biasOff(const saved_hw_t *s)
{ DAC_CR = s->dac; DAC_DHR12R1 = s->dhr; RCC_APBENR1 = s->rcc; GPIOA_MODER = s->moder; }
static void adcSelPA4(const saved_hw_t *s)
{
    ADC_SMPR3 = (s->smpr3 & ~(7u << SMP4_POS)) | (((s->smpr3 >> SMP8_POS) & 7u) << SMP4_POS);
    ADC_SQR3 = (s->sqr3 & ~0x1Fu) | ADC_CH_PA4;
}
static void adcRestore(const saved_hw_t *s){ ADC_SQR3 = s->sqr3; ADC_SMPR3 = s->smpr3; }
static uint16_t adcRead(void)
{
    ADC_CR2 |= ADC_CR2_START;
    for (uint16_t n = 0; !(ADC_SR & ADC_SR_EOC) && n < 2000u; n++) {}
    return (uint16_t)(ADC_DR & 0x0FFFu);
}

/* Main NTC (auto_rx get_Temp): range in bits 12..13, °C from TEMP_TAB asset. */
static void m20Temp(sonde_info_t *in)
{
    uint16_t tab[T_N], a = in->tRaw;
    uint8_t sc = 0;
    if (a > 8191u) { sc = 2; a -= 8192u; }
    else if (a > 4095u) { sc = 1; a -= 4096u; }
    g.A->asset_read((uint16_t)(TEMP_TAB + sc * (T_N * 2u)), tab, sizeof tab);
    for (uint8_t i = 0; i + 1u < T_N; i++) {
        if (a <= tab[i] && a > tab[i + 1u]) {
            in->temp_c10 = (int16_t)(i * 50 - (int)T_OFS10 +
                                     50 * (tab[i] - a) / (tab[i] - tab[i + 1u]));
            in->flags |= SONDE_HAS_TEMP;
            return;
        }
    }
}

static void emit(char *buf, char *end)
{
    *end = '\0';
    g.A->print_normal(buf, 0, 0, g.line++);
}

static void attCycle(int8_t dir)
{
    if (dir < 0)
        g.cfg.att = g.cfg.att ? (uint8_t)(g.cfg.att - 1u) : (uint8_t)(ATT_COUNT - 1u);
    else if (++g.cfg.att >= ATT_COUNT)
        g.cfg.att = 0;
    applyAtt();
    g.A->delay_ms(ATT_SETTLE);
}

static void draw(const char *s)
{
    char str[34]; char *o;
    const sonde_info_t *in = &g.info;
    g.A->display_clear(); g.A->status_clear();
    g.A->draw_battery();
    g.A->print_inverse(s + T_TITLE, 2, 0, true, true, (uint8_t)(2u + T_TITLE_CHARS * 4u));
    if (g.fArm) g.A->asset_read(BMP_F, g.A->status_line + SPK_X, BMP_F_LEN);
    else if (g.cfg.spk) g.A->asset_read(BMP_SPK, g.A->status_line + SPK_X, SPK_W);
    g.line = 1;

    if (g.nOk) {
        if (in->flags & SONDE_HAS_POS) {
            char *latEnd = putPos(str, in->latS, 'N', 'S');
            o = latEnd; *o++ = ' ';
            o = putPos(o, in->lonS, 'E', 'W');
            if ((unsigned)(o - str) > 18u) {
                emit(str, latEnd);
                o = putPos(str, in->lonS, 'E', 'W');
            }
        } else o = put(str, s + T_NOPOS);
        emit(str, o);
        o = put(str, s + T_ALT); o = puti(o, in->alt_m); o = put(o, s + T_M);
        emit(str, o);
        o = str;
        if (in->flags & SONDE_HAS_TEMP) {
            o = put(o, s + T_TEMP); o = put1(o, in->temp_c10); o = put(o, s + T_C);
        }
        if (in->flags & SONDE_HAS_PRESS) { o = put1(o, in->press_h10); o = put(o, s + T_HPA); }
        if (o != str) emit(str, o);
        g.A->print_bold(in->id, 0, 0, 0);
    }
    for (unsigned x = 0; x < 128u; x += 2u) g.A->fb[5][x] |= 0x01u;

    o = putFreq(str, g.vfoFreq + (uint32_t)((int32_t)g.offSteps * STEP_10HZ));
    if (g.offSteps) {
        *o++ = ' '; if (g.offSteps > 0) *o++ = '+';
        o = puti(o, (int32_t)g.offSteps * 5); o = put(o, s + T_KHZ);
    }
    tiny(str, 0, 43, o);
    o = puti(str, g.rssi); o = put(o, s + T_DBM);
    tiny(str, (uint8_t)(128u - (unsigned)(o - str) * 4u), 43, o);
    o = put(str, s + T_FRAME); o = puti(o, g.nOk);
    tiny(str, 0, 49, o);
    o = putAtt(str, s);
    tiny(str, (uint8_t)(128u - (unsigned)(o - str) * 4u), 49, o);
}

static bool handleKey(uint8_t key)
{
    if (key == APP_KEY_INVALID || key == g.prevKey) { g.prevKey = key; return false; }
    g.prevKey = key;
    if (key == APP_KEY_F) {
        g.fArm = !g.fArm;
        return true;
    }
    const bool reverse = g.fArm;
    g.fArm = false;
    switch (key) {
        case APP_KEY_EXIT: g.running = false; break;
        case APP_KEY_1: g.cfg.spk ^= 1u; g.A->audio_path(g.cfg.spk); break;
        case APP_KEY_2: {
            int8_t n = (int8_t)(g.offSteps + (reverse ? -1 : 1));
            if (n >= -OFF_MAX && n <= OFF_MAX) { g.offSteps = n; tune(); }
            break; }
        case APP_KEY_3: attCycle(reverse ? -1 : 1); break;
        case APP_KEY_4: g.offSteps = 0; tune(); break;
        default: break;
    }
    return true;
}

__attribute__((noinline))
static void listen(const saved_hw_t *saved, const char *ui)
{
    decm20_t m20;
    decm20_init(&m20);
    adcSelPA4(saved);
    uint32_t prev = SYST_VAL, cyc = 0, next = 0;
    uint16_t cnt = 0;
    uint8_t armedRefresh = 0;
    while (g.running) {
        uint32_t v;
        do {
            v = SYST_VAL;
            cyc += (v <= prev) ? prev - v : prev + (SYST_LOAD + 1u - v);
            prev = v;
        } while (cyc < next);
        next += CYC_PER_SAMPLE;
        bool wantUi = false;
        if (decm20_push(&m20, adcRead())) {
            sonde_info_t info;
            info.flags = 0;
            info.press_h10 = 0;
            if (decm20_parse(&m20, &info)) {
                m20Temp(&info);
                g.info = info;
                g.nOk++;
                wantUi = true;
            }
            decm20_rearm(&m20);
        }
        /* No UI in ST_DATA: get_key alone exceeds one sample period. */
        if (decm20_busy(&m20)) continue;
        uint16_t keyEvery = g.fArm ? KEY_ARMED_EVERY : KEY_IDLE_EVERY;
        if (!wantUi && ++cnt < keyEvery) continue;
        cnt = 0;
        bool fastPoll = g.fArm;
        bool keyUi = handleKey(g.A->get_key());
        if (!g.running) break;
        if (wantUi || keyUi || !fastPoll || ++armedRefresh >= ARMED_REFRESH_SLOTS) {
            armedRefresh = 0;
            g.rssi = g.A->rssi_dbm();
            adcRestore(saved);
            g.A->battery_sample();
            adcSelPA4(saved);
            draw(ui);
            g.A->blit_status();
            g.A->blit_full();
            g.A->backlight_on();
        }
        /* Key scans and UI work span sample periods; resume from now. */
        decm20_rearm(&m20);
        prev = SYST_VAL;
        next = cyc + CYC_PER_SAMPLE;
    }
    adcRestore(saved);
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    saved_hw_t saved;
    uint8_t cfg[4];
    char ui[UI_SIZE] __attribute__((aligned(4)));
    Gdiv = api; g.A = api; g.prevKey = APP_KEY_INVALID;
    api->asset_read(0, ui, UI_SIZE);

    saved.sqr3 = ADC_SQR3; saved.smpr3 = ADC_SMPR3; saved.moder = GPIOA_MODER;
    saved.dac = DAC_CR; saved.rcc = RCC_APBENR1; saved.dhr = DAC_DHR12R1;
    biasOn(&saved);
    api->backlight_on();
    g.vfoFreq = api->rx_freq();
    api->bk_write(REG_2B, (uint16_t)((api->bk_read(REG_2B) | 0x0700u) & ~0x0007u));
    api->bk_write(REG_73, (uint16_t)(api->bk_read(REG_73) | 0x0010u));
    uint16_t r13 = api->bk_read(BK_REG_13);
    uint16_t r43 = api->bk_read(REG_43);
    api->bk_write(REG_43, REG_43_M20);
    api->cfg_load(cfg, 4);
    if (cfg[0] == CFG_MAGIC) {
        g.cfg.spk = cfg[1] == 1u;
        if (cfg[3] < ATT_COUNT) g.cfg.att = cfg[3];
    } else g.cfg.att = 3u;
    applyAtt();
    api->audio_path(g.cfg.spk);
    api->set_af(APP_AF_FM);
    api->bk_write(REG_47, REG_47_M20);
    api->delay_ms(50);
    g.running = true;
    listen(&saved, ui);
    if (g.offSteps) { g.offSteps = 0; tune(); }
    api->bk_write(BK_REG_13, r13);
    api->set_agc(true);
    api->bk_write(REG_43, r43);
    biasOff(&saved);
    api->set_af(APP_AF_MUTE);
    api->audio_path(false);
    cfg[0] = CFG_MAGIC; cfg[1] = g.cfg.spk; cfg[2] = 0; cfg[3] = g.cfg.att;
    api->cfg_save(cfg, 4);
}
