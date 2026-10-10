#include <stdint.h>
#include <stdbool.h>
#include "ble_status.h"

/* BLE slot status indicators. Pure, ZMK-free; see ble_status.h.
 *
 * Per slot key (F1..F3), highest priority first:
 *   1. a timed animation (connected solid+fade, switch confirm solid+fade,
 *      or red flash), newest wins;
 *   2. the active slot, not connected, blinks (EMPTY, pairing) or breathes
 *      (PAIRED, connecting): with Fn held always, without Fn only while the
 *      output is BLE and for RRGB_BLE_STEADY_HOLD_FRAMES after the slot's
 *      last event (boot/wake, profile select, state change, flash or
 *      switch confirm end);
 *   3. with Fn held: the overview colour.
 * F4 shows the output (white USB / cyan BLE) only with Fn held. The passkey
 * guidance owns the number row and Enter while the host waits for the code;
 * after Enter, keys 1..6 run the verify chase (the pairing slot keeps its
 * blink meanwhile), and a FAILED then flashes them red with the slot.
 *
 * Timing uses wrap-safe tick differences (int32). Expired animation kinds
 * and an expired s_pk_on are never cleared: harmless until the tick has
 * moved ~2^31 frames (~497 days at 50 FPS) past them, when the difference
 * turns negative (still off); only after ~2^32 frames could one reappear.
 *
 * Threads: zmk_adapter.c calls the slot/output/event setters from one work
 * item on the system workqueue and set_fn from the ZMK layer listener;
 * s_guide is written by rrgb_ble_set_passkey_guide() from the settings path
 * (the mcumgr SMP work queue at runtime, the main thread at boot load). One
 * writer per variable; render/active run on the render thread. Single
 * core, so a torn read is a one-frame glitch. */

enum anim_kind { ANIM_NONE = 0, ANIM_CONN, ANIM_FLASH, ANIM_SELECT };

struct slot_anim {
	uint8_t kind;    /* enum anim_kind, kept after expiry (flash/confirm end = blink origin) */
	uint32_t t0;
};

static struct rrgb_ble_keys s_keys;
static volatile uint8_t s_state[RRGB_BLE_SLOTS];
static volatile uint8_t s_active = RRGB_BLE_NONE;
static volatile uint32_t s_steady_t0;    /* active slot's last steady event */
static volatile bool s_first_poll;       /* next set_slots is boot/wake: an event */
static volatile bool s_output_ble;
static volatile bool s_fn;
static struct slot_anim s_anim[RRGB_BLE_SLOTS];

static volatile bool s_pk_on;
static volatile uint8_t s_pk_slot;
static volatile uint8_t s_pk_digits;
static volatile uint32_t s_pk_t0;        /* Enter pulse origin */
static volatile uint32_t s_pk_last;      /* last passkey event, for the safety end */

static volatile bool s_vf_on;            /* passkey submitted, the host verifies it */
static volatile uint8_t s_vf_slot;
static volatile uint32_t s_vf_t0;
static volatile bool s_df_on;            /* keys 1..6 follow the red flash of s_df_slot */
static volatile uint8_t s_df_slot;
static volatile bool s_guide = true;     /* ind.passkey_guide: number row guidance shown */

static int32_t since(uint32_t tick, uint32_t t0) { return (int32_t)(tick - t0); }

static struct rrgb blue(uint8_t v)  { return (struct rrgb){0, 0, v}; }
static struct rrgb white(uint8_t v) { return (struct rrgb){v, v, v}; }
static struct rrgb red(uint8_t v)   { return (struct rrgb){v, 0, 0}; }
static struct rrgb cyan(uint8_t v)  { return (struct rrgb){0, v, v}; }

/* Triangle: RRGB_BLE_BRIGHT at phase 0, 0 at half period. */
static uint8_t tri(int32_t dt, uint32_t period) {
	uint32_t p = (uint32_t)dt % period, h = period / 2;
	uint32_t d = p < h ? h - p : p - h;
	return (uint8_t)(RRGB_BLE_BRIGHT * d / h);
}

static bool anim_running(uint8_t s, uint32_t tick) {
	int32_t dt = since(tick, s_anim[s].t0);
	switch (s_anim[s].kind) {
	case ANIM_CONN:  return dt >= 0 && dt < RRGB_BLE_CONN_SOLID + RRGB_BLE_CONN_FADE;
	case ANIM_FLASH: return dt >= 0 && dt < RRGB_BLE_FLASH_TOTAL;
	case ANIM_SELECT: return dt >= 0 && dt < RRGB_BLE_SELECT_TOTAL;
	default:         return false;
	}
}

static bool pk_running(uint32_t tick) {
	int32_t dt = since(tick, s_pk_last);
	return s_pk_on && dt >= 0 && dt < RRGB_BLE_PASSKEY_MAX;
}

static bool vf_running(uint32_t tick) {
	int32_t dt = since(tick, s_vf_t0);
	return s_vf_on && dt >= 0 && dt < RRGB_BLE_VERIFY_MAX;
}

/* The digit flash mirrors the slot's red flash (same t0), so a repeated
 * FAILED that restarts the slot flash keeps both in sync. */
static bool df_running(uint32_t tick) {
	return s_df_on && s_anim[s_df_slot].kind == ANIM_FLASH && anim_running(s_df_slot, tick);
}

/* Steady phase origin: the last steady event, or the end of a later red
 * flash or switch confirm on the slot (that comes first, then the steady
 * animation). */
static uint32_t steady_origin(uint8_t s) {
	uint32_t origin = s_steady_t0;
	uint32_t len = s_anim[s].kind == ANIM_FLASH  ? RRGB_BLE_FLASH_TOTAL
		     : s_anim[s].kind == ANIM_SELECT ? RRGB_BLE_SELECT_TOTAL : 0;
	if (len) {
		uint32_t end = s_anim[s].t0 + len;
		if (since(end, origin) > 0) { origin = end; }
	}
	return origin;
}

/* Slot s shows its steady (blink/breathe) animation this frame without Fn. */
static bool steady_auto(uint8_t s, uint32_t tick) {
	if (s == s_vf_slot && vf_running(tick)) {
		/* verifying: the pairing slot blinks, also on a re-pair over a bond */
		return true;
	}
	if (s != s_active || s_state[s] == RRGB_BLE_CONNECTED) { return false; }
	int32_t dt = since(tick, steady_origin(s));
	return s_output_ble && dt >= 0 && dt < RRGB_BLE_STEADY_HOLD_FRAMES;
}

/* Slot s shows its steady animation this frame (Fn held: always). */
static bool steady_shown(uint8_t s, uint32_t tick) {
	if (steady_auto(s, tick)) { return true; }
	return s_fn && s == s_active && s_state[s] != RRGB_BLE_CONNECTED;
}

static void start_anim(uint8_t s, uint8_t kind, uint32_t tick) {
	/* The digit flash belongs to one verify failure: a new animation on its
	 * slot after it ended (a later LOST/CLEARED/FAILED) does not bring it back.
	 * A repeated FAILED while it runs keeps it (in sync). */
	if (s_df_on && s_df_slot == s && !df_running(tick)) { s_df_on = false; }
	s_anim[s].t0 = tick;
	s_anim[s].kind = kind;
}

void rrgb_ble_init(const struct rrgb_ble_keys *keys) {
	s_keys = *keys;
	for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
		s_state[s] = RRGB_BLE_EMPTY;
		s_anim[s] = (struct slot_anim){ANIM_NONE, 0};
	}
	s_active = RRGB_BLE_NONE;
	s_steady_t0 = 0;
	s_first_poll = true;
	s_output_ble = false;
	s_fn = false;
	s_pk_on = false;
	s_pk_digits = 0;
	s_vf_on = false;
	s_df_on = false;
}

void rrgb_ble_set_slots(const uint8_t state[3], uint8_t active, uint32_t tick) {
	if (active >= RRGB_BLE_SLOTS) { active = RRGB_BLE_NONE; }
	bool switched = !s_first_poll && active != s_active && active < RRGB_BLE_SLOTS;
	bool steady_changed = s_first_poll || active != s_active;
	s_first_poll = false;
	for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
		uint8_t old = s_state[s], now = state[s];
		if (now == old || now > RRGB_BLE_CONNECTED) { continue; }   /* bad value: ignored */
		if (now == RRGB_BLE_CONNECTED) {
			start_anim(s, ANIM_CONN, tick);
		} else if (old == RRGB_BLE_CONNECTED && now == RRGB_BLE_PAIRED) {
			start_anim(s, ANIM_FLASH, tick);   /* LOST (still bonded) */
		} else if (s_anim[s].kind == ANIM_CONN) {
			/* CONNECTED -> EMPTY is a bond clear, not a LOST: CLEARED flashes */
			s_anim[s].kind = ANIM_NONE;
		}
		s_state[s] = now;
		if (s == active) { steady_changed = true; }
	}
	if (switched && !(s_anim[active].kind == ANIM_CONN && anim_running(active, tick))) {
		start_anim(active, ANIM_SELECT, tick);   /* explicit profile switch: confirm */
	}
	s_active = active;
	if (steady_changed) { s_steady_t0 = tick; }
}

void rrgb_ble_set_output_ble(bool ble) { s_output_ble = ble; }
void rrgb_ble_set_fn(bool held)        { s_fn = held; }
void rrgb_ble_set_passkey_guide(bool on) { s_guide = on; }

static void pk_end(uint8_t slot) {
	if (s_pk_on && s_pk_slot == slot) { s_pk_on = false; }
}

/* Ends the verify chase of slot; returns true if it was running. */
static bool vf_end(uint8_t slot, uint32_t tick) {
	bool was = vf_running(tick) && s_vf_slot == slot;
	if (s_vf_on && s_vf_slot == slot) { s_vf_on = false; }
	return was;
}

void rrgb_ble_event(enum rrgb_ble_ev ev, uint8_t slot, uint8_t arg, uint32_t tick) {
	if (slot >= RRGB_BLE_SLOTS) { return; }
	switch (ev) {
	case RRGB_BLE_EV_LOST:
		pk_end(slot);
		vf_end(slot, tick);
		start_anim(slot, ANIM_FLASH, tick);
		break;
	case RRGB_BLE_EV_FAILED:
		pk_end(slot);
		if (vf_end(slot, tick)) {   /* wrong passkey: keys 1..6 flash with the slot */
			start_anim(slot, ANIM_FLASH, tick);
			s_df_slot = slot;
			s_df_on = true;
		} else {
			start_anim(slot, ANIM_FLASH, tick);
		}
		break;
	case RRGB_BLE_EV_CLEARED:
		pk_end(slot);
		vf_end(slot, tick);
		s_state[slot] = RRGB_BLE_EMPTY;
		if (slot == s_active) { s_steady_t0 = tick; }
		start_anim(slot, ANIM_FLASH, tick);
		break;
	case RRGB_BLE_EV_PASSKEY_REQ:
		s_vf_on = false;
		s_df_on = false;
		s_pk_slot = slot;
		s_pk_digits = 0;
		s_pk_t0 = tick;
		s_pk_last = tick;
		s_pk_on = true;
		break;
	case RRGB_BLE_EV_PASSKEY_DIGITS:
		if (!s_pk_on || s_pk_slot != slot) {   /* missed REQ: start the guidance now */
			s_pk_slot = slot;
			s_pk_t0 = tick;
			s_pk_on = true;
		}
		s_pk_digits = arg > RRGB_BLE_PASSKEY_LEN ? RRGB_BLE_PASSKEY_LEN : arg;
		s_pk_last = tick;
		break;
	case RRGB_BLE_EV_PASSKEY_SUBMITTED:
		pk_end(slot);
		s_vf_slot = slot;
		s_vf_t0 = tick;
		s_vf_on = true;
		s_df_on = false;
		break;
	case RRGB_BLE_EV_PAIRED_OK:
		pk_end(slot);
		vf_end(slot, tick);
		s_state[slot] = RRGB_BLE_CONNECTED;   /* the following poll does not restart it */
		start_anim(slot, ANIM_CONN, tick);
		break;
	}
}

/* The number row guidance (passkey digits, red digit flash) runs and is
 * switched on (ind.passkey_guide). The verify chase counts separately: it
 * also makes its slot blink (steady_auto), which is slot status and stays
 * when the guidance is off. */
static bool guide_running(uint32_t tick) {
	return s_guide && (pk_running(tick) || df_running(tick));
}

bool rrgb_ble_active(uint32_t tick) {
	if (s_fn || guide_running(tick) || vf_running(tick)) { return true; }
	for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
		if (anim_running(s, tick)) { return true; }
	}
	return s_active < RRGB_BLE_SLOTS && steady_shown(s_active, tick);
}

bool rrgb_ble_suppress_effect(uint32_t tick) {
	if (guide_running(tick) || vf_running(tick)) { return true; }
	for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
		if (anim_running(s, tick)) { return true; }
	}
	return s_active < RRGB_BLE_SLOTS && steady_auto(s_active, tick);
}

static bool put(struct rrgb *px, uint16_t n, uint8_t led, struct rrgb c) {
	if (led == RRGB_BLE_NONE || led >= n) { return false; }
	px[led] = c;
	return true;
}

static struct rrgb timed_colour(uint8_t s, uint32_t tick) {
	int32_t dt = since(tick, s_anim[s].t0);
	if (s_anim[s].kind == ANIM_FLASH) {
		bool on = (uint32_t)dt % (RRGB_BLE_FLASH_ON + RRGB_BLE_FLASH_OFF) < RRGB_BLE_FLASH_ON;
		return red(on ? RRGB_BLE_BRIGHT : 0);
	}
	int32_t solid = s_anim[s].kind == ANIM_SELECT ? RRGB_BLE_SELECT_SOLID : RRGB_BLE_CONN_SOLID;
	if (dt < solid) { return blue(RRGB_BLE_BRIGHT); }
	return blue((uint8_t)(RRGB_BLE_BRIGHT *
		(uint32_t)(solid + RRGB_BLE_CONN_FADE - dt) / RRGB_BLE_CONN_FADE));
}

/* Active slot not connected: blink (EMPTY, pairing, or the slot verifying a
 * passkey, also a re-pair over a bond) or breathe (PAIRED, connecting),
 * phase 0 at steady_origin(). */
static struct rrgb steady_colour(uint8_t s, uint32_t tick) {
	int32_t dt = since(tick, steady_origin(s));
	if (dt < 0) { dt = 0; }
	if (s_state[s] == RRGB_BLE_EMPTY || (s == s_vf_slot && vf_running(tick))) {
		bool on = (uint32_t)dt % RRGB_BLE_BLINK_PERIOD < RRGB_BLE_BLINK_ON;
		return blue(on ? RRGB_BLE_BRIGHT : 0);
	}
	return blue(tri(dt, RRGB_BLE_BREATHE_PERIOD));
}

static struct rrgb overview_colour(uint8_t s) {
	switch (s_state[s]) {
	case RRGB_BLE_CONNECTED: return blue(s == s_active ? RRGB_BLE_BRIGHT : RRGB_BLE_BG);
	case RRGB_BLE_PAIRED:    return blue(RRGB_BLE_VDIM);
	default:                 return white(RRGB_BLE_VDIM);
	}
}

bool rrgb_ble_render(struct rrgb *px, uint16_t n, uint32_t tick) {
	bool painted = false;

	for (uint8_t s = 0; s < RRGB_BLE_SLOTS; s++) {
		struct rrgb c;
		if (anim_running(s, tick)) {
			c = timed_colour(s, tick);
		} else if (steady_shown(s, tick)) {
			c = steady_colour(s, tick);
		} else if (s_fn) {
			c = overview_colour(s);
		} else {
			continue;
		}
		painted |= put(px, n, s_keys.slot[s], c);
	}

	if (s_fn) {
		painted |= put(px, n, s_keys.output,
			       s_output_ble ? cyan(RRGB_BLE_OUT) : white(RRGB_BLE_OUT));
	}

	if (!s_guide) { return painted; }   /* ind.passkey_guide off: the number row is not ours */
	if (pk_running(tick)) {
		for (uint8_t k = 0; k < 10; k++) {
			struct rrgb c = k < s_pk_digits ? blue(RRGB_BLE_BRIGHT) : white(RRGB_BLE_DIM);
			painted |= put(px, n, s_keys.numrow[k], c);
		}
		painted |= put(px, n, s_keys.enter,
			       white(tri(since(tick, s_pk_t0), RRGB_BLE_ENTER_PERIOD)));
	} else if (vf_running(tick)) {
		/* chase 1 -> 6: head bright, two keys of trailing fade, rest dark */
		uint32_t dt = (uint32_t)since(tick, s_vf_t0);
		uint8_t head = (uint8_t)((dt % RRGB_BLE_VERIFY_PERIOD) / RRGB_BLE_VERIFY_STEP);
		for (uint8_t k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) {
			uint8_t d = (uint8_t)((head + RRGB_BLE_PASSKEY_LEN - k) % RRGB_BLE_PASSKEY_LEN);
			uint8_t v = d == 0 ? RRGB_BLE_BRIGHT : d == 1 ? RRGB_BLE_WAVE_TAIL1
				  : d == 2 ? RRGB_BLE_WAVE_TAIL2 : 0;
			painted |= put(px, n, s_keys.numrow[k], blue(v));
		}
	} else if (df_running(tick)) {
		struct rrgb c = timed_colour(s_df_slot, tick);
		for (uint8_t k = 0; k < RRGB_BLE_PASSKEY_LEN; k++) {
			painted |= put(px, n, s_keys.numrow[k], c);
		}
	}
	return painted;
}
