#include "../overlay.h"
#include "../led_map.h"
#include "../ble_status.h"
#include "test.h"
#include <string.h>

static int eq(struct rrgb a, struct rrgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static struct rrgb blue(uint8_t v)  { return (struct rrgb){0, 0, v}; }
static struct rrgb white(uint8_t v) { return (struct rrgb){v, v, v}; }
static struct rrgb at(const struct rrgb *px, int pos) { return px[rrgb_led_for_position((uint32_t)pos)]; }

/* The board's layer 1 (rainy75.keymap): bound (not &trans) at 0..14, 28,
 * 43, 56, 65, 72 and 80..82, as zmk_adapter.c reads it from the keymap. */
static const char *real_fn_dev(uint16_t pos, void *ctx) {
    static const uint8_t lit[] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                  12, 13, 14, 28, 43, 56, 65, 72, 80, 81, 82};
    (void)ctx;
    for (unsigned i = 0; i < sizeof(lit); i++) {
        if (lit[i] == pos) { return "rgb"; }
    }
    return "transparent";
}

static void set_real_fn_keys(void) {
    uint32_t m[RRGB_FN_MASK_WORDS];
    rrgb_fn_mask_build(real_fn_dev, NULL, 83, "transparent", m);
    rrgb_overlay_set_fn_keys(m);
}

#define POS_F(i)  (1 + (i))   /* F1..F3 = positions 1..3 */
#define POS_F4    4
#define POS_F5    5
#define POS_NUM(i) (16 + (i)) /* number row 1..0 */
#ifdef CONFIG_RAINY_RGB_ANSI_LEDMAP
#define POS_ENTER 56          /* ANSI wide Enter */
#else
#define POS_ENTER 43          /* ISO Enter */
#endif

/* ble_status integration: render order, ownership, overlay_active. */
static void test_ble(void) {
    struct rrgb px[83];
    const uint8_t all_empty[3] = {RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY};

    rrgb_overlay_init(true);
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_set_battery(0);
    rrgb_overlay_battery_show(0);
    uint32_t t = 1000;                          /* past the battery window */

    /* idle: nothing to draw, nothing touched */
    CHECK(!rrgb_overlay_active(t));
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){7, 7, 7}; }
    rrgb_overlay_render(px, 83, t, 255);
    for (int i = 0; i < 83; i++) { CHECK(eq(px[i], (struct rrgb){7, 7, 7})); }

    /* Fn held, all slots EMPTY, output USB: ble owns F1..F4, the rest of the
     * Fn-highlight stays white (ble does not paint keys it does not own). */
    rrgb_overlay_set_fn(true);
    CHECK(rrgb_overlay_active(t));
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50, 50, 50}; }
    rrgb_overlay_render(px, 83, t, 255);
    for (int s = 0; s < 3; s++) { CHECK(eq(at(px, POS_F(s)), white(RRGB_BLE_VDIM))); }
    CHECK(eq(at(px, POS_F4), white(RRGB_BLE_OUT)));
    CHECK(eq(at(px, 0), white(255)));            /* ESC: Fn white */
    CHECK(eq(at(px, POS_F5), white(255)));       /* F5: Fn white */
    CHECK(eq(at(px, 13), white(255)));           /* BT_CLR key: Fn white */
    CHECK(eq(at(px, 31), white(0)));             /* Q: black */

    /* slot states through the overlay: active connected / other paired, BLE output */
    const uint8_t mixed[3] = {RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY};
    rrgb_ble_set_output_ble(true);
    rrgb_ble_set_slots(mixed, 0, t);
    t += RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE;   /* connected solid+fade over */
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, POS_F(0)), blue(RRGB_BLE_BRIGHT)));
    CHECK(eq(at(px, POS_F(1)), blue(RRGB_BLE_VDIM)));
    CHECK(eq(at(px, POS_F(2)), white(RRGB_BLE_VDIM)));
    CHECK(!eq(at(px, POS_F4), at(px, POS_F(0))));              /* F4 distinct from the slot */
    CHECK(eq(at(px, POS_F4), (struct rrgb){0, RRGB_BLE_OUT, RRGB_BLE_OUT}));   /* cyan */
    CHECK(eq(at(px, POS_F5), white(255)));

    /* Fn released, everything connected and settled: nothing to draw */
    rrgb_overlay_set_fn(false);
    CHECK(!rrgb_overlay_active(t));
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){7, 7, 7}; }
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, POS_F(0)), (struct rrgb){7, 7, 7}));

    /* CapsLock and a ble animation at the same time: both show */
    rrgb_overlay_set_caps(true);
    rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, t);
    CHECK(rrgb_overlay_active(t));
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, 44), white(255)));                          /* CapsLock */
    CHECK(eq(at(px, POS_F(1)), (struct rrgb){RRGB_BLE_BRIGHT, 0, 0}));   /* red flash on */
    rrgb_overlay_set_caps(false);
    t += RRGB_BLE_FLASH_TOTAL;

    /* passkey guidance wins over the battery gauge on the number row */
    rrgb_overlay_set_battery(100);
    rrgb_overlay_battery_show(t);
    rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 2, 0, t);
    rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 2, 2, t);
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){7, 7, 7}; }
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, POS_NUM(0)), blue(RRGB_BLE_BRIGHT)));
    CHECK(eq(at(px, POS_NUM(1)), blue(RRGB_BLE_BRIGHT)));
    for (int k = 2; k < 10; k++) { CHECK(eq(at(px, POS_NUM(k)), white(RRGB_BLE_DIM))); }
    CHECK(eq(at(px, POS_ENTER), white(RRGB_BLE_BRIGHT)));       /* Enter pulse, phase 0 */
    CHECK(eq(at(px, 15), (struct rrgb){7, 7, 7}));              /* ` untouched */
    CHECK(eq(at(px, 26), (struct rrgb){7, 7, 7}));              /* - untouched */

    /* guidance over: the gauge shows again in its window */
    rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 2, 0, t + 1);
    rrgb_overlay_render(px, 83, t + 1, 255);
    CHECK(at(px, POS_NUM(9)).g > 30);                           /* 100 % green */

    /* RGB off with only a ble animation: overlay_active keeps the loop alive */
    t += 1000;                                                  /* all settled */
    CHECK(!rrgb_overlay_active(t));
    rrgb_ble_set_slots(all_empty, 0, t);                        /* active slot 0 cleared */
    CHECK(rrgb_overlay_active(t));                              /* blinking */
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){0, 0, 0}; }
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, POS_F(0)), blue(RRGB_BLE_BRIGHT)));
    rrgb_ble_set_output_ble(false);
    CHECK(!rrgb_overlay_active(t));                             /* USB: steady gated */

    /* explicit profile switch: confirm flash on the new slot, also on USB */
    rrgb_ble_set_slots(all_empty, 2, t);
    CHECK(rrgb_overlay_active(t));
    rrgb_overlay_render(px, 83, t, 255);
    CHECK(eq(at(px, POS_F(2)), blue(RRGB_BLE_BRIGHT)));
    CHECK(!rrgb_overlay_active(t + RRGB_BLE_SELECT_TOTAL));    /* then gated (USB) */

    /* reactive presses: suppressed while the Fn layer is held */
    rrgb_overlay_set_fn(true);
    CHECK(!rrgb_overlay_key_reactive(POS_F(0), t + RRGB_BLE_SELECT_TOTAL));
    CHECK(!rrgb_overlay_key_reactive(31, t + RRGB_BLE_SELECT_TOTAL));
    rrgb_overlay_set_fn(false);
    CHECK(rrgb_overlay_key_reactive(POS_F(0), t + RRGB_BLE_SELECT_TOTAL));
    CHECK(rrgb_overlay_key_reactive(31, t + RRGB_BLE_SELECT_TOTAL));

    /* build without BLE: ble owns nothing, Fn-highlight unchanged */
    rrgb_overlay_init(false);
    rrgb_overlay_set_fn(true);
    rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 0, 0, t);
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50, 50, 50}; }
    rrgb_overlay_render(px, 83, t, 255);
    for (int s = 0; s < 3; s++) { CHECK(eq(at(px, POS_F(s)), white(255))); }
    CHECK(eq(at(px, POS_F4), white(255)));
    CHECK(eq(at(px, POS_NUM(0)), white(0)));
    rrgb_overlay_set_fn(false);
    CHECK(!rrgb_overlay_active(t));         /* ble state ignored without BLE */
    CHECK(!rrgb_overlay_suppress_effect(t));

    rrgb_ble_event(RRGB_BLE_EV_FAILED, 0, 0, t);
    CHECK(!rrgb_overlay_suppress_effect(t));   /* nor its flashes */

    /* with BLE: any automatic BLE animation turns the effect off, and the
     * overlay stays active meanwhile (rail on even with RGB off) */
    rrgb_overlay_init(true);
    CHECK(!rrgb_overlay_suppress_effect(t));
    CHECK(rrgb_overlay_key_reactive(31, t));
    rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t);
    CHECK(rrgb_overlay_suppress_effect(t));
    CHECK(rrgb_overlay_active(t));
    /* presses while suppressed leave no reactive trace (typed digits) */
    CHECK(!rrgb_overlay_key_reactive(31, t));
    rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, t + 1);
    CHECK(rrgb_overlay_suppress_effect(t + RRGB_BLE_FLASH_TOTAL));
    CHECK(rrgb_overlay_active(t + RRGB_BLE_FLASH_TOTAL));
    CHECK(!rrgb_overlay_suppress_effect(t + 1 + RRGB_BLE_FLASH_TOTAL));
    CHECK(rrgb_overlay_key_reactive(31, t + 1 + RRGB_BLE_FLASH_TOTAL));
    /* suppress implies active, frame by frame, through a switch + blink */
    const uint8_t one_empty[3] = {RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY};
    rrgb_ble_set_output_ble(true);
    rrgb_ble_set_slots(one_empty, 0, t + 100);
    rrgb_ble_set_slots(one_empty, 1, t + 300);
    for (uint32_t k = t; k < t + 3000; k++) {
        if (rrgb_overlay_suppress_effect(k)) { CHECK(rrgb_overlay_active(k)); }
    }
    CHECK(rrgb_overlay_suppress_effect(t + 400));
}

/* Effect gain: fast fade out when suppressed, smooth fade back in. */
static void test_effect_gain(void) {
    uint8_t g = 255;
    int n = 0;
    while (g > 0) { g = rrgb_effect_gain_next(g, true); n++; CHECK(n < 100); }
    CHECK(n >= RRGB_EFFECT_FADE_OUT_FRAMES - 1 && n <= RRGB_EFFECT_FADE_OUT_FRAMES);
    CHECK(rrgb_effect_gain_next(0, true) == 0);
    n = 0;
    uint8_t prev = 0;
    while (g < 255) {
        g = rrgb_effect_gain_next(g, false);
        CHECK(g > prev);
        prev = g;
        n++;
        CHECK(n < 100);
    }
    CHECK(n >= RRGB_EFFECT_FADE_IN_FRAMES - 1 && n <= RRGB_EFFECT_FADE_IN_FRAMES);
    CHECK(rrgb_effect_gain_next(255, false) == 255);
    /* suppression mid fade-in turns around from where it is */
    g = rrgb_effect_gain_next(0, false);
    g = rrgb_effect_gain_next(g, false);
    CHECK(rrgb_effect_gain_next(g, true) < g);
}

/* Frames without a drawn effect layer (RGB off, render loop stopped) snap
 * the gain to its target, so it never freezes mid fade: RGB toggled on
 * shows the effect at full gain, or stays dark while still suppressed. */
static void test_effect_gain_frame(void) {
    CHECK(rrgb_effect_gain_frame(128, false, false) == 255);
    CHECK(rrgb_effect_gain_frame(128, true, false) == 0);
    CHECK(rrgb_effect_gain_frame(0, false, false) == 255);
    /* drawn: the normal fade step */
    CHECK(rrgb_effect_gain_frame(128, true, true) == rrgb_effect_gain_next(128, true));
    CHECK(rrgb_effect_gain_frame(128, false, true) == rrgb_effect_gain_next(128, false));
}

/* rrgb_fn_mask_build(): lit unless unbound (NULL) or transparent; &none is
 * a binding (lit); positions past the mask are ignored. */
static const char *table_dev(uint16_t pos, void *ctx) {
    const char *const *t = ctx;
    return pos < 4 ? t[pos] : (pos == 95 ? "kp" : "transparent");
}

static void test_fn_mask_build(void) {
    const char *t[4] = {"kp", "transparent", NULL, "none"};
    uint32_t m[RRGB_FN_MASK_WORDS];

    rrgb_fn_mask_build(table_dev, t, 100, "transparent", m);
    CHECK(m[0] == ((1u << 0) | (1u << 3)));
    CHECK(m[1] == 0);
    CHECK(m[2] == (1u << 31));                   /* position 95; 96..99 ignored */
    rrgb_fn_mask_build(table_dev, t, 100, NULL, m);   /* no &trans in the build */
    CHECK((m[0] & (1u << 1)) != 0);
    CHECK((m[0] & (1u << 2)) == 0);              /* unbound stays dark */
    rrgb_fn_mask_build(table_dev, t, 2, "transparent", m);
    CHECK(m[0] == 1u && m[1] == 0 && m[2] == 0); /* only n positions read */
}

/* ind.fn_highlight off: holding Fn changes nothing (no BLE in this build). */
static void test_fn_highlight_setting(void) {
    struct rrgb px[83];

    rrgb_overlay_init(false);
    rrgb_overlay_set_caps(false);
    rrgb_overlay_battery_show(0);                /* window closed by tick 1000 */
    set_real_fn_keys();
    rrgb_overlay_set_fn(true);
    rrgb_overlay_set_fn_highlight(false);
    CHECK(!rrgb_overlay_active(1000));
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50, 50, 50}; }
    rrgb_overlay_render(px, 83, 1000, 255);
    for (int i = 0; i < 83; i++) { CHECK(eq(px[i], (struct rrgb){50, 50, 50})); }
    rrgb_overlay_set_fn_highlight(true);
    CHECK(rrgb_overlay_active(1000));
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(eq(at(px, 0), white(255)) && eq(at(px, 31), white(0)));
    rrgb_overlay_set_fn(false);
}

/* The highlight follows the key set it is given (the live keymap). */
static void test_fn_keys_live(void) {
    struct rrgb px[83];
    uint32_t m[RRGB_FN_MASK_WORDS] = {0};

    rrgb_overlay_init(false);
    m[31 / 32] = 1u << (31 % 32);                /* only Q bound on layer 1 */
    rrgb_overlay_set_fn_keys(m);
    rrgb_overlay_set_fn(true);
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50, 50, 50}; }
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(eq(at(px, 31), white(255)));
    CHECK(eq(at(px, 0), white(0)));              /* Esc dark now */
    rrgb_overlay_set_fn(false);
    set_real_fn_keys();
}

/* ind.caps_style / ind.caps_color. */
static void test_caps_styles(void) {
    struct rrgb px[83];

    rrgb_overlay_init(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_battery_show(0);
    rrgb_overlay_set_caps(true);
    rrgb_overlay_set_caps_style(RRGB_CAPS_KEY, 0xFF0000);
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){100, 100, 100}; }
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(eq(at(px, 44), (struct rrgb){255, 0, 0}));
    CHECK(eq(at(px, 31), (struct rrgb){100, 100, 100}));
    CHECK(rrgb_overlay_active(1000));
    rrgb_overlay_set_caps_style(RRGB_CAPS_OFF, 0x0000FF);
    CHECK(!rrgb_overlay_active(1000));
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){100, 100, 100}; }
    rrgb_overlay_render(px, 83, 1000, 255);
    for (int i = 0; i < 83; i++) { CHECK(eq(px[i], (struct rrgb){100, 100, 100})); }
    rrgb_overlay_set_caps_style(RRGB_CAPS_KEY, 0xFFFFFF);   /* the defaults again */
    rrgb_overlay_set_caps(false);
}

static void fill(struct rrgb *px, uint8_t v) {
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){v, v, v}; }
}

static int all_eq(const struct rrgb *px, struct rrgb c) {
    for (int i = 0; i < 83; i++) {
        if (!eq(px[i], c)) { return 0; }
    }
    return 1;
}

/* ind.caps_style tint: every LED mixed 50/50 with the colour scaled to the
 * brightness the effect was drawn at (tint_v); nothing over the Fn overview
 * or without the effect, so the tint alone keeps nothing active. */
static void test_caps_tint(void) {
    struct rrgb px[83];

    rrgb_overlay_init(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_battery_show(0);
    rrgb_overlay_set_caps(true);
    rrgb_overlay_set_caps_style(RRGB_CAPS_TINT, 0x0000FF);
    /* v 255, gain 255: the plain 50/50 mix */
    fill(px, 100);
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(all_eq(px, (struct rrgb){50, 50, 177}));
    /* v 0 (effect not drawn): untouched */
    fill(px, 100);
    rrgb_overlay_render(px, 83, 1000, 0);
    CHECK(all_eq(px, (struct rrgb){100, 100, 100}));
    /* half brightness: half the colour (255 * 128 / 255 = 128) */
    fill(px, 100);
    rrgb_overlay_render(px, 83, 1000, 128);
    CHECK(all_eq(px, (struct rrgb){50, 50, (100 + 128) / 2}));
    /* Fn overview: no tint over it */
    rrgb_overlay_set_fn(true);
    fill(px, 50);
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(eq(at(px, 0), white(255)) && eq(at(px, 31), white(0)));
    /* Fn held with the highlight off: no overview, the tint shows */
    rrgb_overlay_set_fn_highlight(false);
    fill(px, 100);
    rrgb_overlay_render(px, 83, 1000, 255);
    CHECK(all_eq(px, (struct rrgb){50, 50, 177}));
    rrgb_overlay_set_fn_highlight(true);
    rrgb_overlay_set_fn(false);
    /* it needs the effect: no frame loop or LED rail for the tint alone */
    CHECK(!rrgb_overlay_active(1000));
    rrgb_overlay_set_caps_style(RRGB_CAPS_KEY, 0xFFFFFF);   /* the defaults again */
    rrgb_overlay_set_caps(false);
}

/* ind.bat_low: the pulse decision. */
static void test_bat_low_alpha(void) {
    CHECK(RRGB_BAT_LOW_PERIOD == 100);                   /* 2 s at 50 FPS */
    CHECK(rrgb_bat_low_alpha(10, 0, false, 0) == 0);     /* off */
    CHECK(rrgb_bat_low_alpha(10, 20, true, 0) == 0);     /* USB host connected */
    CHECK(rrgb_bat_low_alpha(0, 20, false, 0) == 0);     /* no reading yet */
    CHECK(rrgb_bat_low_alpha(20, 20, false, 0) == 0);    /* at the threshold: not below */
    CHECK(rrgb_bat_low_alpha(19, 20, false, 0) == 255);  /* full at phase 0 ... */
    CHECK(rrgb_bat_low_alpha(19, 20, false, RRGB_BAT_LOW_PERIOD / 2) == 0);   /* dark at half */
    CHECK(rrgb_bat_low_alpha(19, 20, false, RRGB_BAT_LOW_PERIOD / 4) == 127);
    CHECK(rrgb_bat_low_alpha(19, 20, false, RRGB_BAT_LOW_PERIOD) == 255);     /* 2 s period */
}

/* The pulse on Esc: blends toward red, touches nothing else, keeps nothing
 * active by itself. */
static void test_bat_low_render(void) {
    struct rrgb px[83];
    int esc = rrgb_led_for_position(0);

    rrgb_overlay_init(false);
    rrgb_overlay_set_battery(10);
    rrgb_overlay_set_bat_low(20);
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){100, 100, 100}; }
    rrgb_overlay_bat_low_render(px, 83, 0, false);
    CHECK(eq(at(px, 0), (struct rrgb){255, 0, 0}));
    CHECK(eq(at(px, 31), (struct rrgb){100, 100, 100}));
    px[esc] = (struct rrgb){100, 100, 100};
    rrgb_overlay_bat_low_render(px, 83, RRGB_BAT_LOW_PERIOD / 2, false);
    CHECK(eq(at(px, 0), (struct rrgb){100, 100, 100}));  /* trough: the effect shows */
    rrgb_overlay_bat_low_render(px, 83, RRGB_BAT_LOW_PERIOD / 4, false);
    CHECK(at(px, 0).r > 100 && at(px, 0).g < 100);        /* half way to red */
    px[esc] = (struct rrgb){100, 100, 100};
    rrgb_overlay_bat_low_render(px, 83, 0, true);
    CHECK(eq(at(px, 0), (struct rrgb){100, 100, 100}));  /* USB host: no pulse */
    rrgb_overlay_battery_show(0);
    CHECK(!rrgb_overlay_active(1000));                   /* not an active overlay */
    rrgb_overlay_set_bat_low(0);
    rrgb_overlay_bat_low_render(px, 83, 0, false);
    CHECK(eq(at(px, 0), (struct rrgb){100, 100, 100}));  /* off */
    rrgb_overlay_set_battery(0);
}

int main(void) {
    struct rrgb px[83];

    set_real_fn_keys();   /* the Fn layer as the adapter reads it */

    /* reset state */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_set_battery(0);

    /* nothing active -> render leaves pixels untouched */
    memset(px, 7, sizeof(px));
    rrgb_overlay_render(px, 83, 100, 255);
    CHECK(px[0].r == 7 && px[10].g == 7);           /* untouched */

    /* CapsLock on -> CapsLock LED (pos 44) is white, others untouched */
    memset(px, 0, sizeof(px));
    rrgb_overlay_set_caps(true);
    rrgb_overlay_render(px, 83, 100, 255);
    int caps_led = rrgb_led_for_position(44);
    CHECK(caps_led >= 0);
    CHECK(px[caps_led].r == 255 && px[caps_led].g == 255 && px[caps_led].b == 255);
    rrgb_overlay_set_caps(false);

    /* Fn active -> only Fn keys lit white, rest black */
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50,50,50}; }
    rrgb_overlay_set_fn(true);
    rrgb_overlay_render(px, 83, 100, 255);
    int led_esc = rrgb_led_for_position(0);    /* ESC: Fn-active */
    int led_q   = rrgb_led_for_position(31);   /* Q: NOT Fn-active */
    CHECK(px[led_esc].r == 255);               /* Fn key lit */
    CHECK((px[led_q].r|px[led_q].g|px[led_q].b) == 0);  /* non-Fn key black */
    rrgb_overlay_set_fn(false);

    /* battery gauge: 50% -> 5 of 10 segments lit on number row (pos 16..25) */
    memset(px, 0, sizeof(px));
    rrgb_overlay_set_battery(50);
    rrgb_overlay_battery_show(0);              /* window until tick 90 */
    rrgb_overlay_render(px, 83, 10, 255);           /* tick 10 < 90 -> active */
    int seg0 = rrgb_led_for_position(16);      /* first segment (lit) */
    int seg9 = rrgb_led_for_position(25);      /* last segment (unlit, dim) */
    CHECK((px[seg0].r|px[seg0].g|px[seg0].b) > 30);   /* lit */
    CHECK(px[seg9].r < 20 && px[seg9].g < 20 && px[seg9].b < 20); /* dim/unlit */

    /* gauge window expires */
    memset(px, 0, sizeof(px));
    rrgb_overlay_render(px, 83, 100, 255);          /* tick 100 >= 90 -> no gauge */
    CHECK((px[seg0].r|px[seg0].g|px[seg0].b) == 0);

    /* rrgb_overlay_active: true while caps/fn/battery-window, else false
       (so indicators render even when decorative RGB is off) */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_battery_show(0);              /* window until tick 90 */
    CHECK(rrgb_overlay_active(10));            /* battery window open */
    CHECK(!rrgb_overlay_active(100));          /* window closed, nothing else */
    rrgb_overlay_set_caps(true);
    CHECK(rrgb_overlay_active(100));           /* caps */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(true);
    CHECK(rrgb_overlay_active(100));           /* fn */
    rrgb_overlay_set_fn(false);
    CHECK(!rrgb_overlay_active(100));          /* nothing active */

    test_ble();
    test_effect_gain();
    test_effect_gain_frame();
    test_fn_mask_build();
    test_fn_highlight_setting();
    test_fn_keys_live();
    test_caps_styles();
    test_caps_tint();
    test_bat_low_alpha();
    test_bat_low_render();
    DONE();
}
