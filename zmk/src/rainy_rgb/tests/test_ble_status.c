#include "../ble_status.h"
#include "test.h"
#include <string.h>

#define N 83
static struct rrgb px[N];

/* Non-default LED indices: nothing in ble_status may assume real positions. */
static const struct rrgb_ble_keys KEYS = {
	.slot = {40, 41, 42},
	.output = 43,
	.numrow = {60, 61, 62, 63, 64, 65, 66, 67, 68, 69},
	.enter = 70,
};
#define F(i)   (KEYS.slot[i])
#define F4     (KEYS.output)
#define NUM(i) (KEYS.numrow[i])
#define ENTER  (KEYS.enter)

static const struct rrgb SENT = {7, 7, 7};

static int eq(struct rrgb a, struct rrgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static struct rrgb blue(uint8_t v)  { return (struct rrgb){0, 0, v}; }
static struct rrgb white(uint8_t v) { return (struct rrgb){v, v, v}; }
static struct rrgb red(uint8_t v)   { return (struct rrgb){v, 0, 0}; }
static struct rrgb cyan(uint8_t v)  { return (struct rrgb){0, v, v}; }
#define SEL RRGB_BLE_SELECT_TOTAL
static const struct rrgb BLACK = {0, 0, 0};

static bool frame(uint32_t tick) {
	for (int i = 0; i < N; i++) { px[i] = SENT; }
	return rrgb_ble_render(px, N, tick);
}

/* All pixels except the listed LEDs are untouched. */
static int only_touched(const uint8_t *leds, int count) {
	for (int i = 0; i < N; i++) {
		int owned = 0;
		for (int k = 0; k < count; k++) { if (leds[k] == i) { owned = 1; } }
		if (!owned && !eq(px[i], SENT)) { return 0; }
	}
	return 1;
}

static void slots(uint8_t a, uint8_t b, uint8_t c, uint8_t active, uint32_t tick) {
	const uint8_t s[3] = {a, b, c};
	rrgb_ble_set_slots(s, active, tick);
}

/* Breathe/pulse triangle: bright at phase 0, dark at half period. */
static uint8_t tri(uint32_t dt, uint32_t period) {
	uint32_t p = dt % period, h = period / 2;
	uint32_t d = p < h ? h - p : p - h;
	return (uint8_t)(RRGB_BLE_BRIGHT * d / h);
}

static void reset(void) {
	rrgb_ble_init(&KEYS);
	rrgb_ble_set_output_ble(true);   /* steady animations need BLE output */
	/* reach a quiet baseline: slot 0 connected and its solid/fade over.
	 * Slot 0 connects at tick 0 (the boot poll), so its connected
	 * animation, and with it rrgb_ble_suppress_effect(), runs from tick 0
	 * and has ended by QUIET. */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, 0);
}
static void boot(void) {
	rrgb_ble_init(&KEYS);
	rrgb_ble_set_output_ble(true);
}
#define QUIET 1000u   /* tick after the baseline's connected animation */

static void check_red_flash(uint8_t slot, uint32_t t0);

static void test_timing_constants(void) {
	CHECK(RRGB_BLE_BLINK_PERIOD == 12 && RRGB_BLE_BLINK_ON == 6);
	CHECK(RRGB_BLE_BREATHE_PERIOD == 50);
	CHECK(RRGB_BLE_CONN_SOLID == 100 && RRGB_BLE_CONN_FADE == 25);
	CHECK(RRGB_BLE_FLASH_COUNT == 3 && RRGB_BLE_FLASH_ON == 8 && RRGB_BLE_FLASH_OFF == 8);
	CHECK(RRGB_BLE_FLASH_TOTAL == 48);
	CHECK(RRGB_BLE_ENTER_PERIOD == 50);
	CHECK(RRGB_BLE_STEADY_HOLD_FRAMES == 1500);
}

static void test_idle(void) {
	rrgb_ble_init(&KEYS);
	/* after init: no active slot shown, nothing to draw */
	CHECK(!rrgb_ble_active(0));
	CHECK(!frame(0));
	CHECK(only_touched(NULL, 0));

	reset();
	CHECK(!rrgb_ble_active(QUIET));
	CHECK(!frame(QUIET));
	CHECK(only_touched(NULL, 0));
}

static void test_fn_overview(void) {
	reset();
	/* slot0 active connected, slot1 connected (background), slot2 paired */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, 0, 0);
	uint32_t t = QUIET;
	rrgb_ble_set_output_ble(false);
	rrgb_ble_set_fn(true);
	CHECK(rrgb_ble_active(t));
	CHECK(frame(t));
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BG)));
	CHECK(eq(px[F(2)], blue(RRGB_BLE_VDIM)));
	CHECK(eq(px[F4], white(RRGB_BLE_OUT)));      /* output USB */
	const uint8_t owned[] = {F(0), F(1), F(2), F4};
	CHECK(only_touched(owned, 4));

	rrgb_ble_set_output_ble(true);
	frame(t);
	CHECK(eq(px[F4], cyan(RRGB_BLE_OUT)));       /* output BLE, not the slot blue */

	/* other slot EMPTY: very dim white; active slot 1 connected */
	slots(RRGB_BLE_EMPTY, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, t);
	frame(t + 500);
	CHECK(eq(px[F(0)], white(RRGB_BLE_VDIM)));
	CHECK(eq(px[F(2)], white(RRGB_BLE_VDIM)));

	/* active EMPTY under Fn: fast blink bright blue */
	slots(RRGB_BLE_EMPTY, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 2, 2000);
	frame(2000 + SEL);                          /* after the switch confirm */
	CHECK(eq(px[F(2)], blue(RRGB_BLE_BRIGHT)));
	frame(2000 + SEL + RRGB_BLE_BLINK_ON);
	CHECK(eq(px[F(2)], BLACK));
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BG)));    /* other connected */

	/* active PAIRED under Fn: breathing */
	slots(RRGB_BLE_PAIRED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, 3000);
	frame(3000 + SEL + 10);
	CHECK(eq(px[F(0)], blue(tri(10, RRGB_BLE_BREATHE_PERIOD))));

	rrgb_ble_set_fn(false);
	rrgb_ble_set_output_ble(false);
}

/* Brightness levels chosen with the user (2026-10-05). */
static void test_levels(void) {
	CHECK(RRGB_BLE_BRIGHT == 255 && RRGB_BLE_BG == 20 && RRGB_BLE_VDIM == 8);
	CHECK(RRGB_BLE_OUT == 102 && RRGB_BLE_DIM == 38);
	CHECK(RRGB_BLE_SELECT_SOLID == 50 && SEL == 75);
}

/* Explicit profile switch: the new slot confirms with solid 1 s + fade. */
static void test_switch_confirm(void) {
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, QUIET);  /* bg connect */
	uint32_t t = QUIET + 500;
	CHECK(!rrgb_ble_active(t));
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, t);      /* Fn+F2 */
	const uint8_t owned[] = {F(1)};
	for (uint32_t dt = 0; dt < SEL; dt++) {
		CHECK(rrgb_ble_active(t + dt));
		CHECK(frame(t + dt));
		uint8_t want = dt < RRGB_BLE_SELECT_SOLID ? RRGB_BLE_BRIGHT
			: (uint8_t)(RRGB_BLE_BRIGHT * (SEL - dt) / RRGB_BLE_CONN_FADE);
		CHECK(eq(px[F(1)], blue(want)));
		CHECK(only_touched(owned, 1));     /* the old slot F1 stays dark */
	}
	CHECK(!rrgb_ble_active(t + SEL));
	CHECK(!frame(t + SEL));

	/* and back to slot 0 */
	t += 1000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, t);
	frame(t + 1);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[F(1)], SENT));

	/* switch onto a slot in its connected solid: the longer solid is kept */
	t += 1000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, 0, t);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, 2, t + 10);
	frame(t + 80);
	CHECK(eq(px[F(2)], blue(RRGB_BLE_BRIGHT)));

	/* re-selecting the active slot (same index) is no switch */
	t += 1000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, 2, t);
	CHECK(!rrgb_ble_active(t));

	/* boot / wake (first poll) is no switch: a connected slot just shows its connect */
	boot();
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, 100);
	frame(110);
	CHECK(eq(px[F(0)], blue(tri(10, RRGB_BLE_BREATHE_PERIOD))));
}

static void test_active_empty_blinks(void) {
	reset();
	uint32_t t0 = 5000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t0);
	const uint8_t owned[] = {F(1)};
	frame(t0);
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));   /* switch confirm first */
	CHECK(only_touched(owned, 1));
	t0 += SEL;                                    /* blink phase 0 at the confirm end */
	for (uint32_t dt = 0; dt < 3 * RRGB_BLE_BLINK_PERIOD; dt++) {
		CHECK(rrgb_ble_active(t0 + dt));
		CHECK(frame(t0 + dt));
		struct rrgb want = (dt % RRGB_BLE_BLINK_PERIOD) < RRGB_BLE_BLINK_ON
			? blue(RRGB_BLE_BRIGHT) : BLACK;
		CHECK(eq(px[F(1)], want));
		CHECK(only_touched(owned, 1));   /* not F4, not the other slots */
	}
	/* still blinking until the hold expires */
	CHECK(rrgb_ble_active(t0 + RRGB_BLE_STEADY_HOLD_FRAMES - 1));
	CHECK(!rrgb_ble_active(t0 + RRGB_BLE_STEADY_HOLD_FRAMES));
}

static void test_active_paired_breathes(void) {
	boot();   /* fresh poll: slot 0 PAIRED without a LOST from CONNECTED */
	uint32_t t0 = 7000;
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0);
	CHECK(eq((frame(t0), px[F(0)]), blue(RRGB_BLE_BRIGHT)));   /* bright at phase 0 */
	frame(t0 + RRGB_BLE_BREATHE_PERIOD / 2);
	CHECK(eq(px[F(0)], BLACK));                                /* dark at half period */
	for (uint32_t dt = 0; dt < 2 * RRGB_BLE_BREATHE_PERIOD; dt++) {
		frame(t0 + dt);
		CHECK(eq(px[F(0)], blue(tri(dt, RRGB_BLE_BREATHE_PERIOD))));
	}
	frame(t0 + RRGB_BLE_BREATHE_PERIOD);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	/* non-active PAIRED slot is not animated without Fn */
	CHECK(eq(px[F(1)], SENT) && eq(px[F(2)], SENT));
}

static void test_connected_solid_fade(void) {
	reset();
	uint32_t t0 = 9000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 1, t0 - 10); /* connecting */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, t0);   /* connected */
	for (uint32_t dt = 0; dt < RRGB_BLE_CONN_SOLID; dt++) {
		CHECK(frame(t0 + dt));
		CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));
	}
	int prev = 256;
	for (uint32_t dt = RRGB_BLE_CONN_SOLID; dt < RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE; dt++) {
		CHECK(rrgb_ble_active(t0 + dt));
		CHECK(frame(t0 + dt));
		uint8_t want = (uint8_t)(RRGB_BLE_BRIGHT *
			(RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE - dt) / RRGB_BLE_CONN_FADE);
		CHECK(eq(px[F(1)], blue(want)));
		CHECK(want < prev);
		prev = want;
	}
	CHECK(eq((frame(t0 + RRGB_BLE_CONN_SOLID), px[F(1)]), blue(RRGB_BLE_BRIGHT)));
	uint32_t end = t0 + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE;
	CHECK(!rrgb_ble_active(end));
	CHECK(!frame(end));
	CHECK(only_touched(NULL, 0));

	/* a slot that stays connected does not restart the animation */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, end + 5);
	CHECK(!rrgb_ble_active(end + 5));

	/* a background slot connecting also gets solid/fade */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, 1, end + 10);
	frame(end + 10);
	CHECK(eq(px[F(2)], blue(RRGB_BLE_BRIGHT)));

	/* connected then dropped (polled, no LOST event): solid replaced by the LOST flash */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, 1, end + 20);
	check_red_flash(2, end + 20);
}

static void check_red_flash(uint8_t slot, uint32_t t0) {
	for (uint32_t dt = 0; dt < RRGB_BLE_FLASH_TOTAL; dt++) {
		CHECK(rrgb_ble_active(t0 + dt));
		CHECK(frame(t0 + dt));
		uint32_t p = dt % (RRGB_BLE_FLASH_ON + RRGB_BLE_FLASH_OFF);
		CHECK(eq(px[F(slot)], p < RRGB_BLE_FLASH_ON ? red(RRGB_BLE_BRIGHT) : BLACK));
	}
	/* exactly COUNT rising edges */
	int edges = 0;
	bool was_on = false;
	for (uint32_t dt = 0; dt < RRGB_BLE_FLASH_TOTAL + 100; dt++) {
		frame(t0 + dt);
		bool on = eq(px[F(slot)], red(RRGB_BLE_BRIGHT));
		if (on && !was_on) { edges++; }
		was_on = on;
	}
	CHECK(edges == RRGB_BLE_FLASH_COUNT);
}

static void test_red_flash(void) {
	/* LOST on the active slot: red flash, then breathing (polled PAIRED) */
	reset();
	uint32_t t0 = 20000;
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 0, 0, t0);
	check_red_flash(0, t0);
	uint32_t after = t0 + RRGB_BLE_FLASH_TOTAL;
	frame(after);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));            /* breathing, phase 0 */
	frame(after + RRGB_BLE_BREATHE_PERIOD / 2);
	CHECK(eq(px[F(0)], BLACK));

	/* LOST on a non-active slot: red flash, then nothing */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 1, 0, QUIET);
	check_red_flash(1, QUIET);
	CHECK(!rrgb_ble_active(QUIET + RRGB_BLE_FLASH_TOTAL));
	CHECK(!frame(QUIET + RRGB_BLE_FLASH_TOTAL));

	/* FAILED on the active empty slot: red flash, then fast blink */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, QUIET + 3);
	check_red_flash(1, QUIET + 3);
	after = QUIET + 3 + RRGB_BLE_FLASH_TOTAL;
	frame(after);
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));
	frame(after + RRGB_BLE_BLINK_ON);
	CHECK(eq(px[F(1)], BLACK));

	/* CLEARED on the active paired slot: red flash, then fast blink (slot now EMPTY
	 * even before the next set_slots) */
	reset();
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_CLEARED, 0, 0, QUIET);
	check_red_flash(0, QUIET);
	after = QUIET + RRGB_BLE_FLASH_TOTAL;
	frame(after);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	frame(after + RRGB_BLE_BLINK_ON);
	CHECK(eq(px[F(0)], BLACK));
	frame(after + RRGB_BLE_BLINK_PERIOD);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));

	/* red flash wins over the Fn overview colour too (newest event per slot) */
	reset();
	rrgb_ble_set_fn(true);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 2, 0, QUIET);
	frame(QUIET);
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));
	frame(QUIET + RRGB_BLE_FLASH_TOTAL);
	CHECK(eq(px[F(2)], white(RRGB_BLE_VDIM)));   /* back to the overview colour */
	rrgb_ble_set_fn(false);

	/* newest event wins: a connect during a red flash replaces it */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 1, 0, QUIET);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, QUIET + RRGB_BLE_FLASH_ON);
	frame(QUIET + RRGB_BLE_FLASH_ON);
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));

	/* slot index out of range is ignored */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_LOST, 3, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 4, 0, QUIET);
	CHECK(!rrgb_ble_active(QUIET));
}

static void test_passkey(void) {
	reset();
	uint32_t t0 = 30000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t0 - 2000);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, t0 - 1000); /* link up */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t0);
	CHECK(rrgb_ble_active(t0));
	CHECK(frame(t0));
	for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], white(RRGB_BLE_DIM))); }
	CHECK(eq(px[ENTER], white(RRGB_BLE_BRIGHT)));       /* pulse phase 0 */
	const uint8_t owned[] = {NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6),
				 NUM(7), NUM(8), NUM(9), ENTER};
	CHECK(only_touched(owned, 11));

	/* Enter pulses at 1 Hz */
	for (uint32_t dt = 0; dt < 2 * RRGB_BLE_ENTER_PERIOD; dt++) {
		frame(t0 + dt);
		CHECK(eq(px[ENTER], white(tri(dt, RRGB_BLE_ENTER_PERIOD))));
	}

	/* digits 0..6: n progress keys bright blue, the rest dim white */
	for (uint8_t d = 0; d <= RRGB_BLE_PASSKEY_LEN; d++) {
		rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, d, t0 + 100 + d);
		frame(t0 + 100 + d);
		for (int k = 0; k < 10; k++) {
			struct rrgb want = k < d ? blue(RRGB_BLE_BRIGHT) : white(RRGB_BLE_DIM);
			CHECK(eq(px[NUM(k)], want));
		}
	}
	/* digits beyond 6 clamp to 6 */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, 9, t0 + 200);
	frame(t0 + 200);
	CHECK(eq(px[NUM(5)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(6)], white(RRGB_BLE_DIM)));
	/* backspace: fewer digits */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, 2, t0 + 201);
	frame(t0 + 201);
	CHECK(eq(px[NUM(1)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(2)], white(RRGB_BLE_DIM)));

	/* PAIRED_OK: guidance ends, slot solid then fade */
	uint32_t ok = t0 + 300;
	rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 1, 0, ok);
	frame(ok);
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(0)], SENT) && eq(px[ENTER], SENT));
	frame(ok + RRGB_BLE_CONN_SOLID + 1);
	CHECK(px[F(1)].b < RRGB_BLE_BRIGHT && px[F(1)].b > 0);
	CHECK(!rrgb_ble_active(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));
	/* the polled CONNECTED that follows does not restart the solid */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1,
	      ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE);
	CHECK(!rrgb_ble_active(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));

	/* FAILED during guidance: guidance ends, red flash on the slot */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 2, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 2, 3, QUIET + 10);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 2, 0, QUIET + 20);
	frame(QUIET + 20);
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(0)], SENT) && eq(px[ENTER], SENT));
	CHECK(!rrgb_ble_active(QUIET + 20 + RRGB_BLE_FLASH_TOTAL));

	/* passkey guidance has a safety end */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 0, 0, QUIET);
	CHECK(rrgb_ble_active(QUIET + RRGB_BLE_PASSKEY_MAX - 1));
	CHECK(!rrgb_ble_active(QUIET + RRGB_BLE_PASSKEY_MAX));
	CHECK(!frame(QUIET + RRGB_BLE_PASSKEY_MAX));
}

/* Chase on the passkey progress keys (1..6) while the host verifies. */
static struct rrgb chase(uint32_t dt, int k) {
	int head = (int)((dt % RRGB_BLE_VERIFY_PERIOD) / RRGB_BLE_VERIFY_STEP);
	int d = (head - k + RRGB_BLE_PASSKEY_LEN) % RRGB_BLE_PASSKEY_LEN;
	return blue(d == 0 ? RRGB_BLE_BRIGHT : d == 1 ? RRGB_BLE_WAVE_TAIL1
		    : d == 2 ? RRGB_BLE_WAVE_TAIL2 : 0);
}

/* The slot key blinks at 4 Hz over one blink period (phase free). */
static int slot_blinks(uint8_t slot, uint32_t t) {
	int on = 0, off = 0;
	for (uint32_t dt = 0; dt < RRGB_BLE_BLINK_PERIOD; dt++) {
		frame(t + dt);
		if (eq(px[F(slot)], blue(RRGB_BLE_BRIGHT))) { on++; }
		else if (eq(px[F(slot)], BLACK)) { off++; }
	}
	return on == RRGB_BLE_BLINK_ON && off == RRGB_BLE_BLINK_PERIOD - RRGB_BLE_BLINK_ON;
}

/* Passkey submitted (Enter): the number row guidance ends, keys 1..6 run a
 * chase 1 -> 6 while the slot keeps its fast blink, until PAIRED_OK (slot
 * solid + fade, number row back) or FAILED (slot and keys 1..6 flash red). */
static void test_verifying(void) {
	CHECK(RRGB_BLE_VERIFY_PERIOD == 30);           /* one sweep per 0.6 s */
	CHECK(RRGB_BLE_VERIFY_STEP * RRGB_BLE_PASSKEY_LEN == RRGB_BLE_VERIFY_PERIOD);
	CHECK(RRGB_BLE_WAVE_TAIL1 < RRGB_BLE_BRIGHT && RRGB_BLE_WAVE_TAIL2 < RRGB_BLE_WAVE_TAIL1);
	CHECK(RRGB_BLE_VERIFY_MAX == 2000);            /* safety end 40 s */

	reset();
	uint32_t t0 = 40000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t0 - 2000);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t0);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, 6, t0 + 50);
	uint32_t ts = t0 + 60;
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, ts);
	/* USB output and the steady hold long over: the blink still shows */
	rrgb_ble_set_output_ble(false);
	CHECK(rrgb_ble_active(ts));
	for (uint32_t dt = 0; dt < 2 * RRGB_BLE_VERIFY_PERIOD; dt++) {
		CHECK(frame(ts + dt));
		for (int k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) { CHECK(eq(px[NUM(k)], chase(dt, k))); }
		for (int k = RRGB_BLE_PASSKEY_LEN; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
		CHECK(eq(px[ENTER], SENT));                 /* Enter pulse ended */
	}
	frame(ts);
	const uint8_t owned[] = {NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), F(1)};
	CHECK(only_touched(owned, 7));
	CHECK(slot_blinks(1, ts + 7));
	CHECK(slot_blinks(1, ts + RRGB_BLE_STEADY_HOLD_FRAMES + 40));
	CHECK(rrgb_ble_active(ts + RRGB_BLE_VERIFY_MAX - 1));

	/* PAIRED_OK: chase ends, slot solid then fade, number row back */
	uint32_t ok = ts + 300;
	rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 1, 0, ok);
	for (uint32_t dt = 0; dt < RRGB_BLE_CONN_SOLID; dt += 7) {
		frame(ok + dt);
		CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));
		for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
	}
	CHECK(!rrgb_ble_active(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));

	/* FAILED (wrong code): slot and keys 1..6 flash red together 3x, then
	 * the number row is back and the active empty slot blinks */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, QUIET + 10);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET + 20);
	uint32_t tf = QUIET + 40;
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf);
	for (uint32_t dt = 0; dt < RRGB_BLE_FLASH_TOTAL; dt++) {
		CHECK(frame(tf + dt));
		uint32_t p = dt % (RRGB_BLE_FLASH_ON + RRGB_BLE_FLASH_OFF);
		struct rrgb want = p < RRGB_BLE_FLASH_ON ? red(RRGB_BLE_BRIGHT) : BLACK;
		CHECK(eq(px[F(1)], want));
		for (int k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) { CHECK(eq(px[NUM(k)], want)); }
		for (int k = RRGB_BLE_PASSKEY_LEN; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
	}
	frame(tf + RRGB_BLE_FLASH_TOTAL);
	for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
	CHECK(slot_blinks(1, tf + RRGB_BLE_FLASH_TOTAL));
	rrgb_ble_set_output_ble(false);
	CHECK(!rrgb_ble_active(tf + RRGB_BLE_FLASH_TOTAL + RRGB_BLE_STEADY_HOLD_FRAMES));

	/* a FAILED for another slot leaves the chase running */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 2, 0, QUIET + 3);
	frame(QUIET + 3);
	CHECK(eq(px[NUM(0)], chase(3, 0)));
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));

	/* connection lost while verifying: chase ends, slot red flash only */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 2, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 2, 0, QUIET + 5);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 2, 0, QUIET + 9);
	check_red_flash(2, QUIET + 9);
	frame(QUIET + 9);
	for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }

	/* safety end: no answer within RRGB_BLE_VERIFY_MAX */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	uint32_t tv = QUIET + 100;
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, tv);
	rrgb_ble_set_output_ble(false);   /* no steady animation behind it */
	CHECK(rrgb_ble_active(tv + RRGB_BLE_VERIFY_MAX - 1));
	CHECK(!rrgb_ble_active(tv + RRGB_BLE_VERIFY_MAX));
	CHECK(!frame(tv + RRGB_BLE_VERIFY_MAX));

	/* out of range slot ignored */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 3, 0, QUIET);
	CHECK(!rrgb_ble_active(QUIET));
}

/* Review fixes: re-pair over a bonded slot blinks during verify; repeated
 * FAILED events keep the digit flash in sync with the slot flash. */
static void test_verify_review(void) {
	/* re-pair over a PAIRED slot (host reconnects, polled CONNECTED) */
	reset();
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 0, 0, QUIET + 10);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, QUIET + 11);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 0, 0, QUIET + 20);
	uint32_t t = QUIET + 11 + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE;  /* conn anim over */
	CHECK(slot_blinks(0, t));
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t + 20);   /* polled PAIRED */
	CHECK(slot_blinks(0, t + 20 + RRGB_BLE_FLASH_TOTAL));                /* blink, not breathe */

	/* FAILED twice (same tick, then tick + 1): digits stay in sync with the slot */
	for (uint32_t gap = 0; gap <= 1; gap++) {
		reset();
		slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
		rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET + 5);
		uint32_t tf = QUIET + 40;
		rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf);
		rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf + gap);
		for (uint32_t dt = gap; dt < RRGB_BLE_FLASH_TOTAL + 10; dt++) {
			frame(tf + dt);
			for (int k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) {
				CHECK(eq(px[NUM(k)], dt < RRGB_BLE_FLASH_TOTAL + gap ? px[F(1)] : SENT));
			}
		}
	}
}

/* The digit flash belongs to the verify failure only: a later flash on the
 * same slot (LOST, CLEARED, FAILED, the open slot timeout) leaves keys 1..6. */
static void test_digit_flash_once(void) {
	for (int ev = 0; ev < 3; ev++) {
		reset();
		slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
		rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET + 5);
		uint32_t tf = QUIET + 40;
		rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf);
		frame(tf);
		CHECK(eq(px[NUM(0)], red(RRGB_BLE_BRIGHT)));
		uint32_t t2 = tf + RRGB_BLE_FLASH_TOTAL + 1500;   /* 30 s later */
		enum rrgb_ble_ev e = ev == 0 ? RRGB_BLE_EV_FAILED : ev == 1 ? RRGB_BLE_EV_LOST
				   : RRGB_BLE_EV_CLEARED;
		rrgb_ble_event(e, 1, 0, t2);
		for (uint32_t dt = 0; dt < RRGB_BLE_FLASH_TOTAL; dt++) {
			frame(t2 + dt);
			for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
		}
		CHECK(eq((frame(t2), px[F(1)]), red(RRGB_BLE_BRIGHT)));
	}
	/* a polled LOST (CONNECTED -> PAIRED) later does not flash the digits either */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET + 5);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, QUIET + 40);
	uint32_t t3 = QUIET + 40 + RRGB_BLE_FLASH_TOTAL + 10;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 1, t3);
	frame(t3);
	CHECK(eq(px[F(1)], red(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(0)], SENT));
}

/* The normal effect is off whenever an automatic BLE animation is visible
 * (no Fn needed): switch confirm, connected solid + fade, red flashes,
 * blink/breathe of the active slot while shown (BLE output, hold window),
 * passkey guidance, verify chase, digit flash. The Fn overview alone and
 * gated (not shown) steady animations keep it. */
static void test_suppress_effect(void) {
	#define SUP(t) rrgb_ble_suppress_effect(t)
	rrgb_ble_init(&KEYS);
	CHECK(!SUP(0));

	/* baseline: slot 0 connects at 0, solid + fade, then the effect returns */
	reset();
	CHECK(SUP(0) && SUP(RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE - 1));
	CHECK(!SUP(RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE) && !SUP(QUIET));
	for (uint32_t t = 0; t < QUIET; t += 7) { CHECK(SUP(t) == rrgb_ble_active(t)); }

	/* Fn overview alone: no suppression */
	rrgb_ble_set_fn(true);
	CHECK(rrgb_ble_active(QUIET) && !SUP(QUIET));
	rrgb_ble_set_fn(false);

	/* switch to an empty slot: confirm, then pairing blink for the hold
	 * window; off the whole time, back after the window */
	uint32_t ts = QUIET;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, ts);
	CHECK(SUP(ts) && SUP(ts + SEL - 1) && SUP(ts + SEL));
	uint32_t hold_end = ts + SEL + RRGB_BLE_STEADY_HOLD_FRAMES;
	CHECK(SUP(hold_end - 1) && !SUP(hold_end));
	/* Fn after the window shows the blink again, but that is not automatic */
	rrgb_ble_set_fn(true);
	CHECK(rrgb_ble_active(hold_end) && !SUP(hold_end));
	rrgb_ble_set_fn(false);

	/* connecting breathe (PAIRED active) on BLE output: off while shown */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, QUIET);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 1, QUIET + 100);
	CHECK(SUP(QUIET + 100 + SEL + 500));
	/* ... gated on USB output: only the switch confirm suppresses */
	rrgb_ble_set_output_ble(false);
	CHECK(!SUP(QUIET + 100 + SEL + 500));
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 2, QUIET + 1000);
	CHECK(SUP(QUIET + 1000 + SEL - 1) && !SUP(QUIET + 1000 + SEL));

	/* full passkey pairing on an empty slot: off from the blink through the
	 * request, digits, chase, until PAIRED_OK solid + fade ended */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	uint32_t t = QUIET + 100;
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t);
	CHECK(SUP(t) && SUP(t + 500));
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, 6, t + 600);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, t + 700);
	/* the chase outlasts the hold window (shown regardless of it) */
	CHECK(SUP(t + 700) && SUP(QUIET + SEL + RRGB_BLE_STEADY_HOLD_FRAMES + 10));
	uint32_t ok = QUIET + SEL + RRGB_BLE_STEADY_HOLD_FRAMES + 20;
	rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 1, 0, ok);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, ok + 3);
	CHECK(SUP(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE - 1));
	CHECK(!SUP(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));

	/* wrong code: off until the red flash (slot + keys 1..6) ended; a
	 * duplicate FAILED one frame later extends it with the flash; the slot
	 * blink after the flash is still within its hold window */
	reset();
	rrgb_ble_set_output_ble(false);   /* no steady: isolate the flash */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, QUIET + 10);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 0, QUIET + 20);
	uint32_t tf = QUIET + 140;
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, tf + 1);
	CHECK(SUP(tf + RRGB_BLE_FLASH_TOTAL));
	CHECK(!SUP(tf + 1 + RRGB_BLE_FLASH_TOTAL));
	rrgb_ble_set_output_ble(true);
	CHECK(SUP(tf + 1 + RRGB_BLE_FLASH_TOTAL));   /* BLE: the blink follows */

	/* passkey timeout / Esc (FAILED during the request) */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 2, 0, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 2, 0, QUIET + 50);
	CHECK(SUP(QUIET + 50 + RRGB_BLE_FLASH_TOTAL - 1));
	CHECK(!SUP(QUIET + 50 + RRGB_BLE_FLASH_TOTAL));

	/* every red flash, also on a background slot: lost, cleared */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, QUIET);
	uint32_t tl = QUIET + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE;
	CHECK(!SUP(tl));
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, tl);   /* LOST */
	CHECK(SUP(tl) && SUP(tl + RRGB_BLE_FLASH_TOTAL - 1) && !SUP(tl + RRGB_BLE_FLASH_TOTAL));
	rrgb_ble_event(RRGB_BLE_EV_CLEARED, 1, 0, tl + 500);
	CHECK(SUP(tl + 500) && !SUP(tl + 500 + RRGB_BLE_FLASH_TOTAL));

	/* Just Works: blink, then PAIRED_OK solid + fade */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 1, 0, QUIET + 300);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, QUIET + 302);
	CHECK(SUP(QUIET + 300 + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE - 1));
	CHECK(!SUP(QUIET + 300 + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));

	/* safety ends */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 0, 0, QUIET);
	CHECK(SUP(QUIET + RRGB_BLE_PASSKEY_MAX - 1) && !SUP(QUIET + RRGB_BLE_PASSKEY_MAX));
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 0, 0, QUIET);
	CHECK(SUP(QUIET + RRGB_BLE_VERIFY_MAX - 1) && !SUP(QUIET + RRGB_BLE_VERIFY_MAX));
	#undef SUP
}

/* Just Works (host without display, no passkey request): fast blink goes
 * straight to PAIRED_OK solid + fade, the number row and Enter untouched. */
static void test_just_works(void) {
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	CHECK(slot_blinks(1, QUIET + SEL + 1));
	uint32_t ok = QUIET + 300;
	rrgb_ble_event(RRGB_BLE_EV_PAIRED_OK, 1, 0, ok);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, ok + 3);
	for (uint32_t t = QUIET; t < ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE; t++) {
		frame(t);
		for (int k = 0; k < 10; k++) { CHECK(eq(px[NUM(k)], SENT)); }
		CHECK(eq(px[ENTER], SENT));
		if (t >= ok && t < ok + RRGB_BLE_CONN_SOLID) { CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT))); }
	}
	CHECK(!rrgb_ble_active(ok + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE));
}

/* Open slot timeout (the module event rainy75_ble_open_profile_timeout maps
 * to FAILED): the open slot flashes red while the slot the keyboard returns
 * to shows the switch confirm. */
static void test_pairing_timeout(void) {
	reset();
	/* slot 0 connected, user selected the empty slot 1: fast blink */
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, QUIET);
	uint32_t t = QUIET + 1500;   /* 30 s later */
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, t);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t);
	for (uint32_t dt = 0; dt < RRGB_BLE_FLASH_TOTAL; dt++) {
		frame(t + dt);
		uint32_t p = dt % (RRGB_BLE_FLASH_ON + RRGB_BLE_FLASH_OFF);
		CHECK(eq(px[F(1)], p < RRGB_BLE_FLASH_ON ? red(RRGB_BLE_BRIGHT) : BLACK));
		if (dt < RRGB_BLE_SELECT_SOLID) { CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT))); }
	}
	/* afterwards: the open slot is no longer active, so it stays dark */
	CHECK(!rrgb_ble_active(t + SEL));
	CHECK(!frame(t + SEL));
}

static void test_multi_slot(void) {
	reset();
	uint32_t t0 = 40000;
	/* slot 0 active pairing (blink), slot 1 connects (solid), slot 2 lost (red) */
	slots(RRGB_BLE_EMPTY, RRGB_BLE_PAIRED, RRGB_BLE_CONNECTED, 0, t0 - 1);
	slots(RRGB_BLE_EMPTY, RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, 0, t0);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 2, 0, t0);
	frame(t0);
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));
	frame(t0 + 8);
	CHECK(eq(px[F(0)], BLACK));                 /* blink off */
	CHECK(eq(px[F(1)], blue(RRGB_BLE_BRIGHT)));  /* solid */
	CHECK(eq(px[F(2)], BLACK));                 /* flash off */
	frame(t0 + 16);
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));
}

static void test_active_exact(void) {
	/* rrgb_ble_active(t) == rrgb_ble_render(t) painted something, over a scenario */
	reset();
	uint32_t t0 = 50000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, t0);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 1, 0, t0 + 5);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 2, 0, t0 + 20);
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 2, 0, t0 + 70);
	for (uint32_t t = t0; t < t0 + 300; t++) {
		bool a = rrgb_ble_active(t);
		bool p = frame(t);
		CHECK(a == p);
		int touched = 0;
		for (int i = 0; i < N; i++) { if (!eq(px[i], SENT)) { touched = 1; } }
		CHECK(touched == (int)p);
	}
	CHECK(!rrgb_ble_active(t0 + 300));
}

static void test_bounds(void) {
	/* positions >= n or NONE are skipped, no out-of-range writes */
	struct rrgb_ble_keys k = KEYS;
	k.slot[0] = RRGB_BLE_NONE;
	k.output = 200;
	rrgb_ble_init(&k);
	rrgb_ble_set_fn(true);
	struct rrgb small[50];
	for (int i = 0; i < 50; i++) { small[i] = SENT; }
	CHECK(!rrgb_ble_render(small, 30, 0));   /* every owned LED is >= 30 */
	for (int i = 0; i < 50; i++) { CHECK(eq(small[i], SENT)); }
	rrgb_ble_set_fn(false);
}


static bool f_on(uint8_t s) { return eq(px[F(s)], blue(RRGB_BLE_BRIGHT)); }
#define HOLD RRGB_BLE_STEADY_HOLD_FRAMES

static void test_steady_gating(void) {
	/* output USB: no steady animation, nothing to draw */
	reset();
	rrgb_ble_set_output_ble(false);
	uint32_t t0 = 60000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t0);
	CHECK(rrgb_ble_active(t0));                /* the switch confirm ignores the output */
	CHECK(rrgb_ble_active(t0 + SEL - 1));
	CHECK(!rrgb_ble_active(t0 + SEL));
	CHECK(!frame(t0 + SEL));
	CHECK(only_touched(NULL, 0));
	t0 += SEL;
	/* Fn held on USB: the overview still blinks the active slot */
	rrgb_ble_set_fn(true);
	frame(t0);
	CHECK(f_on(1));
	frame(t0 + RRGB_BLE_BLINK_ON);
	CHECK(eq(px[F(1)], BLACK));
	rrgb_ble_set_fn(false);
	/* output BLE: shown (window from the slot event at t0) */
	rrgb_ble_set_output_ble(true);
	CHECK(rrgb_ble_active(t0 + 1));
	frame(t0 + 1);
	CHECK(f_on(1));
	/* event animations ignore the output */
	rrgb_ble_set_output_ble(false);
	rrgb_ble_event(RRGB_BLE_EV_LOST, 2, 0, t0 + 10);
	frame(t0 + 10);
	CHECK(eq(px[F(2)], red(RRGB_BLE_BRIGHT)));
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_CONNECTED, 1, t0 + 100);
	frame(t0 + 100);
	CHECK(f_on(2));
	CHECK(eq(px[F(1)], SENT));                  /* steady still gated */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t0 + 300);
	frame(t0 + 300);
	CHECK(eq(px[NUM(0)], white(RRGB_BLE_DIM)));
}

static void test_steady_hold(void) {
	/* breathing for HOLD frames after the event, then dark */
	boot();   /* fresh poll: slot 0 PAIRED without a LOST from CONNECTED */
	uint32_t t0 = 70000;
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0);
	CHECK(rrgb_ble_active(t0 + HOLD - 1));
	frame(t0 + HOLD - 1);
	CHECK(eq(px[F(0)], blue(tri(HOLD - 1, RRGB_BLE_BREATHE_PERIOD))));
	CHECK(!rrgb_ble_active(t0 + HOLD));
	CHECK(!frame(t0 + HOLD));
	CHECK(only_touched(NULL, 0));
	/* a repeated poll with the same state is no event */
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0 + HOLD + 10);
	CHECK(!rrgb_ble_active(t0 + HOLD + 10));
	/* Fn after expiry: the overview still breathes the active slot */
	rrgb_ble_set_fn(true);
	CHECK(rrgb_ble_active(t0 + HOLD + 20));
	frame(t0 + HOLD + 20);
	CHECK(eq(px[F(0)], blue(tri(HOLD + 20, RRGB_BLE_BREATHE_PERIOD))));
	rrgb_ble_set_fn(false);

	/* profile select restarts the window: EMPTY slot becomes active */
	uint32_t t1 = t0 + 5000;
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t1);
	frame(t1);
	CHECK(f_on(1));
	CHECK(rrgb_ble_active(t1 + SEL + HOLD - 1) && !rrgb_ble_active(t1 + SEL + HOLD));

	/* CLEARED on the active slot: flash, then blink for HOLD from the flash end */
	uint32_t t2 = t1 + 5000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t2 - 1000);
	rrgb_ble_event(RRGB_BLE_EV_CLEARED, 0, 0, t2);
	uint32_t e = t2 + RRGB_BLE_FLASH_TOTAL;
	frame(e);
	CHECK(f_on(0));
	CHECK(rrgb_ble_active(e + HOLD - 1) && !rrgb_ble_active(e + HOLD));

	/* disconnect (polled LOST) of the active slot: flash, then breathe for HOLD */
	uint32_t t3 = e + 5000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t3 - 1000);
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t3);
	e = t3 + RRGB_BLE_FLASH_TOTAL;
	frame(e);
	CHECK(f_on(0));
	CHECK(rrgb_ble_active(e + HOLD - 1) && !rrgb_ble_active(e + HOLD));

	/* boot/wake: the first set_slots after init starts the window */
	rrgb_ble_init(&KEYS);
	rrgb_ble_set_output_ble(true);
	uint32_t t4 = 90000;
	slots(RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t4);
	frame(t4);
	CHECK(f_on(0));
	CHECK(rrgb_ble_active(t4 + HOLD - 1) && !rrgb_ble_active(t4 + HOLD));
	/* boot with an EMPTY active slot (init state is EMPTY too): still an event */
	rrgb_ble_init(&KEYS);
	rrgb_ble_set_output_ble(true);
	slots(RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t4);
	frame(t4);
	CHECK(f_on(0));
	CHECK(rrgb_ble_active(t4 + HOLD - 1) && !rrgb_ble_active(t4 + HOLD));
}

static void test_lost_detection(void) {
	/* CONNECTED -> PAIRED in the poll: LOST red flash */
	reset();
	uint32_t t0 = 100000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 0, t0 - 1000);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_PAIRED, RRGB_BLE_EMPTY, 0, t0);
	check_red_flash(1, t0);

	/* CLEARED event then the poll: one flash from the CLEARED tick, no LOST restart */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0 - 1000);
	rrgb_ble_event(RRGB_BLE_EV_CLEARED, 0, 0, t0);
	slots(RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0 + 20);
	frame(t0 + 16);
	CHECK(eq(px[F(0)], red(RRGB_BLE_BRIGHT)));   /* 3rd flash of the CLEARED flash */
	frame(t0 + 20);
	CHECK(eq(px[F(0)], red(RRGB_BLE_BRIGHT)));   /* restart at 20 would be on too ... */
	frame(t0 + 24);
	CHECK(eq(px[F(0)], BLACK));                  /* ... but this is the CLEARED off phase */
	frame(t0 + RRGB_BLE_FLASH_TOTAL);
	CHECK(f_on(0));                              /* blink after the single flash */

	/* poll first (CONNECTED -> EMPTY means the bond is gone): no LOST flash */
	reset();
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0 - 1000);
	slots(RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 0, t0);
	frame(t0);
	CHECK(f_on(0));                              /* blink, not red */
	frame(t0 + RRGB_BLE_BLINK_ON);
	CHECK(eq(px[F(0)], BLACK));
	rrgb_ble_event(RRGB_BLE_EV_CLEARED, 0, 0, t0 + 10);
	frame(t0 + 10);
	CHECK(eq(px[F(0)], red(RRGB_BLE_BRIGHT)));

	/* explicit LOST still works */
	reset();
	rrgb_ble_event(RRGB_BLE_EV_LOST, 0, 0, t0);
	check_red_flash(0, t0);

	/* state values > 2 are ignored */
	reset();
	slots(RRGB_BLE_CONNECTED, 7, 200, 0, t0);
	CHECK(!rrgb_ble_active(t0));
	rrgb_ble_set_fn(true);
	frame(t0);
	CHECK(eq(px[F(1)], white(RRGB_BLE_VDIM)));   /* still EMPTY */
	CHECK(eq(px[F(2)], white(RRGB_BLE_VDIM)));
	rrgb_ble_set_fn(false);
}

static void test_wraparound(void) {
	uint32_t w = 0xFFFFFFF0u;
	/* connected solid across the wrap */
	rrgb_ble_init(&KEYS);
	rrgb_ble_set_output_ble(true);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, w);
	frame(w + 50);                              /* tick 34 after the wrap */
	CHECK(eq(px[F(0)], blue(RRGB_BLE_BRIGHT)));
	CHECK(rrgb_ble_active(w + RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE - 1));
	/* blink of the active EMPTY slot across the wrap */
	for (uint32_t dt = 0; dt < 40; dt++) {
		frame(w + dt);
		CHECK(eq(px[F(1)], (dt % RRGB_BLE_BLINK_PERIOD) < RRGB_BLE_BLINK_ON
				  ? blue(RRGB_BLE_BRIGHT) : BLACK));
	}
	CHECK(rrgb_ble_active(w + HOLD - 1) && !rrgb_ble_active(w + HOLD));
	/* red flash across the wrap */
	rrgb_ble_event(RRGB_BLE_EV_LOST, 2, 0, 0xFFFFFFFAu);
	check_red_flash(2, 0xFFFFFFFAu);
	/* passkey across the wrap, incl. its safety end */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, 0xFFFFFFFFu);
	frame(5);
	CHECK(eq(px[NUM(0)], white(RRGB_BLE_DIM)));
	CHECK(eq(px[ENTER], white(tri(6, RRGB_BLE_ENTER_PERIOD))));
	rrgb_ble_set_output_ble(false);   /* only the passkey guidance left */
	CHECK(rrgb_ble_active(0xFFFFFFFFu + RRGB_BLE_PASSKEY_MAX - 1));
	CHECK(!rrgb_ble_active(0xFFFFFFFFu + RRGB_BLE_PASSKEY_MAX));
	frame(0xFFFFFFFFu + RRGB_BLE_PASSKEY_MAX);
	CHECK(eq(px[NUM(0)], SENT));
	/* very old events stay off (up to 2^31 frames later) */
	rrgb_ble_set_output_ble(true);
	CHECK(!rrgb_ble_active(0x7FFFFFF0u));
	CHECK(!frame(0x7FFFFFF0u));
}

/* ind.passkey_guide off: the number row and Enter stay untouched through a
 * whole passkey pairing, the slot keys keep their status. */
static void test_passkey_guide_off(void) {
	reset();
	rrgb_ble_set_passkey_guide(false);
	uint32_t t0 = 30000;
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, RRGB_BLE_EMPTY, 1, t0 - 2000);
	slots(RRGB_BLE_CONNECTED, RRGB_BLE_CONNECTED, RRGB_BLE_EMPTY, 1, t0 - 1000);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t0);
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_DIGITS, 1, 3, t0 + 10);
	CHECK(!rrgb_ble_active(t0 + 10));            /* nothing else to show */
	CHECK(!rrgb_ble_suppress_effect(t0 + 10));   /* the effect stays on */
	CHECK(!frame(t0 + 10));
	CHECK(eq(px[NUM(0)], SENT) && eq(px[ENTER], SENT));

	/* Enter: no chase on 1..6, the slot blinks (slot status) */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_SUBMITTED, 1, 6, t0 + 20);
	CHECK(rrgb_ble_active(t0 + 20));
	CHECK(slot_blinks(1, t0 + 20));
	for (int k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) { CHECK(eq(px[NUM(k)], SENT)); }

	/* wrong code: the slot flashes red, keys 1..6 do not */
	rrgb_ble_event(RRGB_BLE_EV_FAILED, 1, 0, t0 + 40);
	frame(t0 + 40);
	CHECK(eq(px[F(1)], red(RRGB_BLE_BRIGHT)));
	CHECK(eq(px[NUM(0)], SENT));

	/* switched on during a new request: shown at once */
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 1, 0, t0 + 100);
	rrgb_ble_set_passkey_guide(true);
	frame(t0 + 100);
	CHECK(eq(px[NUM(0)], white(RRGB_BLE_DIM)));

	/* a setting, not state: rrgb_ble_init() keeps it */
	rrgb_ble_set_passkey_guide(false);
	reset();
	rrgb_ble_event(RRGB_BLE_EV_PASSKEY_REQ, 0, 0, QUIET);
	CHECK(!frame(QUIET));
	rrgb_ble_set_passkey_guide(true);
}

int main(void) {
	test_timing_constants();
	test_idle();
	test_levels();
	test_switch_confirm();
	test_fn_overview();
	test_active_empty_blinks();
	test_active_paired_breathes();
	test_connected_solid_fade();
	test_red_flash();
	test_passkey();
	test_verifying();
	test_passkey_guide_off();
	test_just_works();
	test_verify_review();
	test_digit_flash_once();
	test_suppress_effect();
	test_pairing_timeout();
	test_multi_slot();
	test_active_exact();
	test_bounds();
	test_steady_gating();
	test_steady_hold();
	test_lost_detection();
	test_wraparound();
	DONE();
}
