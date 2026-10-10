# rainy_rgb — Out-of-Tree RGB Lighting Engine

Custom WS2812 lighting engine for the Rainy 75, built as an **out-of-tree Zephyr
module** (everything under `zmk/`, no edits to `zmk-src/`/`zephyr/`). It **replaces
ZMK's built-in `rgb_underglow`** (`CONFIG_ZMK_RGB_UNDERGLOW=n`) and owns the
`led_strip` device directly, so it survives ZMK upgrades — the only coupling to ZMK
internals is confined to two files (`zmk_adapter.c`, `behavior_rainy_rgb.c`).

Hardware-verified end to end on the physical keyboard (USB + BLE, mcuboot DFU).

## Files (`zmk/src/rainy_rgb/`)

| File | Responsibility |
|------|----------------|
| `color.{h,c}` | Pure 8-bit color math: `hsv2rgb`, `sin8`, `scale8`, `hypot8`. Host-tested. |
| `effects.{h,c}` | `struct rgb_frame`, the effect registry, and all effect render functions (12 display effects, plus an opt-in `walker` diagnostic). Pure. |
| `engine.{h,c}` | Owns `pixels[83]` + a dedicated **50 FPS render thread**; runtime state; the FPS-independent speed model; settings load; idle timer, battery cap and USB host state; dispatch (effect → overlay → strip). |
| `lighting.{h,c}` | Brightness policy of the effect layer: battery cap (`rgb.val_battery`) and idle off/dim (`rgb.idle_s`, `rgb.idle_mode`). Pure, host-tested. |
| `reactive.{h,c}` | Lock-free **SPSC press queue** (event thread → render thread) feeding an 8-slot ripple pool + per-LED `key_heat[83]`. |
| `overlay.{h,c}` | Functional indicators (CapsLock style / Fn-highlight / battery gauge / low-battery pulse / BLE slot status). Pure, ZMK-free. Owns the keymap-coupled key positions (CapsLock, Esc, BLE keys); the Fn-highlight keys arrive as a mask built from the live keymap. |
| `ble_status.{h,c}` | BLE slot status on F1..F4, passkey guidance on the number row and Enter: state machine + renderer, timing and brightness constants. Pure, ZMK-free, host-tested. |
| `led_map.{h,c}` | Calibrated `pos_to_led[83]` + `led_positions[83]` (XY) + lookups. ISO and ANSI table variants (`CONFIG_RAINY_RGB_ANSI_LEDMAP`, set by `./build.sh --ansi`). |
| `state.c` | NVS persistence (`SETTINGS_STATIC_HANDLER`, subtree `rainy_rgb/`, 2 s debounce). |
| `zmk_adapter.{h,c}` | **ZMK boundary**: led_strip wrap + `ZMK_LISTENER`/`ZMK_SUBSCRIPTION` for position/layer/hid-indicators/battery/USB → neutral setters; the Fn-highlight key mask from the live keymap; BLE profile/endpoint/auth events and BT connection callbacks → `ble_status` (one work item). |
| `../behaviors/behavior_rainy_rgb.c` | **ZMK boundary**: the `&rgb` keymap behavior → engine API. |
| `tests/test_{color,effects,overlay,ble_status,lighting}.c` | Host gcc unit tests (run `tests/run_host_tests.sh`). |

The board DTS exposes the strip as `chosen zmk,underglow = &led_strip` (driver
`telink,b91-spi-led-strip`, PB7 MOSI, DMA ch4, ~6 MHz, GRB, PC2 = LED VCC MOSFET).

## Render pipeline (per frame, render thread only)

```
reactive_tick (drain key presses → ripples + heat)
  → effect renders into pixels[]  (at min(val, val_battery) without a USB host, a quarter
                                    of that while idle dim; black base if RGB is off or
                                    idle off; scaled by the effect gain, black while a BLE
                                    animation shows)
  → low-battery pulse on Esc  (only while the effect is drawn)
  → overlay_render  (Fn-highlight base-override, then CapsLock, then battery gauge,
                     then BLE slot status last)
  → led_strip_update_rgb  (~2.66 ms DMA; render thread sleeps on the End-IRQ)
```

`led_strip_update_rgb` **sleeps** the render thread on a semaphore given by the
PSPI End-of-Transfer interrupt (PLIC source 23) for the ~2.66 ms DMA transfer —
no busy-poll, so the CPU is free during the transfer (a 5 ms timeout + frame-skip
guards against a missed IRQ). It still runs only on the dedicated low-priority
render thread, never from an ISR/event callback. Frame rate is 50 FPS
(deadline-paced); the thread is preemptible by BLE/system threads.

## Effects (12, cycle with Fn+Enter)

`solid · rainbow · plasma · twinkle · comet · aurora · reactive · ripple · wave · rain · heatmap · speedcolour`

- **Ambient:** solid, rainbow, plasma, twinkle, comet, aurora, wave (diagonal), rain (drops fall by Y).
- **Reactive:** reactive (global pulse on keypress), ripple (concurrent rainbow rings expanding from the pressed key), heatmap (keys glow on press, cool over time), speedcolour (board-wide colour deepens with typing speed — after the stock firmware's *Trigger Colour* mode; hue picks the colour, sat/val cap the deep end).
- **Diagnostic (opt-in):** `walker` — enable `CONFIG_RAINY_RGB_WALKER` to append a 13th effect that lights exactly one LED (white) and steps to the next chain index on each keypress, wrapping. It walks the raw WS2812 chain `0..N-1`, independent of the ISO/ANSI `led_map`, so it works on either board — watch which key lights, press any key to advance. Off by default; enable only when calibrating the LED order.
- `fire` and `calibrate` existed earlier and were removed on request.

Spatial effects (ripple/wave/rain/heatmap) use the calibrated `led_positions[]` XY map.

Fn+Enter steps through the runtime setting `rgb.cycle` (default: all effects in table
order): the next entry after the current effect, or the first entry when the current
effect is not in the list; an empty list means all effects. Example:
`python3 reverse/tools/rainy75_cfg.py set rgb.cycle solid,plasma,wave`
(see [config-protocol.md](config-protocol.md)).

## Controls (Fn layer)

| Combo | Action |
|-------|--------|
| Fn+Backspace | RGB toggle (on/off) |
| Fn+Enter | next effect of `rgb.cycle` |
| Fn+# (NUHS) | hue |
| Fn+↑ / Fn+↓ | brightness |
| Fn+→ / Fn+← | speed |
| Fn+B | battery gauge (3 s) |

State (on/off, effect, hue, sat, val, speed) persists to NVS (subtree `rainy_rgb/`,
2 s save debounce).

## Animation speed (FPS-independent)

Ambient effects advance off a shared `.8` fixed-point **phase accumulator**
(`engine.c`), not the raw frame counter, so animation speed is **decoupled from
the frame rate** — raising FPS makes motion smoother, never faster — and is
**identical across all effects**. The speed knob (Fn+→/←, 1..255) maps to
phase-units/second; the per-frame increment is that `/ RRGB_FPS`. Sub-unit
increments let the slowest step truly crawl (~34 s per color cycle) where an
integer `tick × factor` could never go below one unit/frame; the fastest stays
controlled (~1.7 s). Tune the range with `RRGB_SPEED_MIN_UPS_Q8` /
`RRGB_SPEED_SLOPE_UPS_Q8`. Reactive timings that key off the frame counter
(reactive pulse, ripple expansion, rain fall, heat decay) are FPS-compensated
constants tuned for 50 FPS.

## XY calibration (Phase 2)

The WS2812 chain order ≠ keymap position order in the nav cluster. A one-time
hardware calibration mapped each LED to the key above it; the result is baked into
`led_map.c` as `pos_to_led[83]` + `led_positions[83]` (XY derived from the board's
`zmk,physical-layout`, uniform-scaled so ripples stay circular). The chain is
identity except: after ISO-Enter it routes `→ PGUP → CapsLock…NUHS → PGDN`.

**ANSI variant** (`CONFIG_RAINY_RGB_ANSI_LEDMAP`, set automatically by
`./build.sh --ansi`): the ANSI PCB's WS2812 chain has **81 LEDs** — it omits the LED
under the ISO `<>` (NUBS) slot and the one under the key right of Space — so ANSI
builds select their own `pos_to_led[]`/XY tables (calibrated on real ANSI hardware,
issue #4). The Enter-cluster detour is identical on both PCBs; the two LED-less
keymap positions park on their neighbor's LED so XY lookups stay physically true.

The calibration *mode* (lit-one-LED-at-a-time + serial dump) was removed after use
(pre-release, not in the public history). To re-calibrate, the quickest ground truth
is a temporary walker effect: light a single LED, step the chain index on any
keypress, note which key each index sits under.

## Functional indicators (Phase 4, overlay)

Rendered on top of the active effect — and **still shown when RGB is toggled off**
(they are functional, not decorative):

- **CapsLock** → by `ind.caps_style`: `key` (default) lights the CapsLock key in
  `ind.caps_color` (default white) at full strength, `tint` mixes every LED 50/50 with the
  colour scaled to the effect's brightness (its rendered value after the battery cap and
  idle dim, times the effect gain of the BLE fade; `tint_v` of `rrgb_overlay_render()`),
  `off` shows nothing (`hid_indicators_changed`, bit 1). The tint is skipped while the Fn
  overview shows and whenever the effect is not drawn (RGB off, idle off, faded out for
  BLE, host direct mode), so it never keeps the LED rail on by itself. Requires
  `CONFIG_ZMK_HID_INDICATORS=y`. Over BLE, some hosts never send the LED report, so caps
  may not update on BLE.
- **Fn-highlight** (`ind.fn_highlight`, default on) → while Fn (layer id 1) is held, the
  keys whose layer-1 binding is not `&trans` light white, the rest dark. The key set comes
  from ZMK's live keymap (`zmk_keymap_get_layer_binding_at_idx()` in `zmk_adapter.c`),
  rebuilt each time layer 1 becomes active, so it follows ZMK Studio edits (also unsaved
  ones, a discard, and Studio's "restore stock settings", which raises no event). Off:
  holding Fn leaves the lighting as it is; the BLE status still shows on F1..F4.
- **Low battery** (`ind.bat_low`, 0..50 %, default 0 = off) → while no USB host is
  connected and the battery level is below the threshold, Esc pulses red (2 s period) on
  top of the effect. Only while the effect is drawn (RGB on, not idle off, no BLE
  suppression, no host mode): the pulse never keeps the LED rail on by itself. A level of 0
  (ZMK's value before its first battery sample) never pulses.
- **Battery gauge** (Fn+B) → a 10-segment bar on the number row, level-colored
  (green→red), 3 s (150 frames). **Approximate** — the battery-ADC pin/divider/Vref are not yet
  hardware-validated (see Open items).
- **BLE slot status** (F1..F3 = BT profiles 1..3, F4 = output), see below.

### BLE slot status and passkey guidance

The three BT profile slots show their state on F1..F3, so pairing, connecting,
switching and failures are visible without a host tool. Slot states: EMPTY (no
bond), PAIRED (bond, not connected), CONNECTED. Colours use fixed levels in
`ble_status.h`, independent of the RGB brightness, and show with RGB off too:
`RRGB_BLE_BRIGHT` 255 (active slot, every animation, passkey digits),
`RRGB_BLE_BG` 20 (~8 %, background connected slot), `RRGB_BLE_VDIM` 8 (~3 %,
paired or empty slot), `RRGB_BLE_OUT` 102 (~40 %, F4), `RRGB_BLE_DIM` 38
(~15 %, number row waiting for digits). WS2812 perceived brightness is far
from linear: the first few counts above 0 are clearly visible, while 60 % and
100 % look almost the same. A first device test with 153 / 38 / 13 made the
active slot hard to tell from the background slot, so the active slot now runs
at full scale and the background levels sit near the bottom (user tuning
2026-10-05).

| When | Key | Shows |
|---|---|---|
| active slot EMPTY (advertising for pairing) | its F-key | bright blue fast blink, 4 Hz |
| active slot PAIRED, not connected (connecting) | its F-key | bright blue breathing, 1 Hz |
| slot becomes CONNECTED (or pairing completes) | its F-key | solid bright blue 2 s, fade 0.5 s |
| explicit profile switch (Fn+F1..F3 changes the active slot) | new slot's F-key | solid bright blue 1 s, fade 0.5 s (also when already connected), then its steady animation |
| connection lost, pairing failed or cancelled | its F-key | red flash 3x (about 1 s), then the steady animation |
| bond cleared (Fn+Del = `BT_CLR`, clears the active slot) | its F-key | red flash 3x, then fast blink |
| host asks for the passkey | 1..0, Enter | number row dim white, keys 1..6 turn bright blue per digit typed, Enter pulses 1 Hz |
| passkey submitted (Enter), host verifies it | 1..6, its F-key | bright blue chase 1 -> 6, one sweep per 0.6 s with a two-key trailing fade; the slot keeps its fast blink |
| wrong passkey (FAILED after Enter) | 1..6, its F-key | keys 1..6 flash red 3x together with the slot, then normal |
| empty slot selected, no host within 30 s (open profile timeout, our module) | old and returning F-key | the empty slot flashes red 3x, the returning slot shows the switch confirm |
| Fn held | F1..F3 | active+connected full blue, other connected dim blue (~8 %), other paired very dim blue (~3 %), empty very dim white (~3 %); the active slot keeps blinking/breathing while not connected |
| Fn held | F4 | white = USB output, cyan = BLE output (~40 %, distinct from the slot blue) |

Blink and breathing (steady animations) show without Fn only while the output
is BLE and for 30 s (`RRGB_BLE_STEADY_HOLD_FRAMES`) after the slot's last event
(boot/wake, profile select, state change, end of a red flash or of the switch
confirm, which comes first); after that the key
stays dark and Fn shows the state. Event animations (red flash, connected fade,
passkey guidance, verify chase) always show (the number-row part only with `ind.passkey_guide`
on). The newest event per slot wins; several slots can
animate at once (multilink). The passkey guidance ends on Enter (passkey
submitted), on pairing complete or failure, and at the latest 60 s after the
last passkey event. After Enter the verify chase runs until pairing complete
(slot solid + fade), failure (red flash on the slot and keys 1..6) or a lost
connection, at the latest 40 s (`RRGB_BLE_VERIFY_MAX`, the SMP timeout is 30 s);
meanwhile the pairing slot blinks regardless of output and hold window, also
on a re-pair over a bonded slot. The red digit flash mirrors the slot's flash
(same start), so repeated `FAILED` events keep both in sync. It belongs to that
one failure: a later flash on the slot (LOST, CLEARED, FAILED, the open slot
timeout) flashes the slot only. ZMK
keeps every key away from the hosts from the passkey request until the pairing
ends (patch 0007), so a second Enter during the check does not reach the PC.
Just Works pairings (no passkey request) never touch the number row. Selecting
a slot (Fn+F1..F3, the module behavior `&bt_sel_ble`) while the output is USB
switches the output to BLE. Selecting an empty slot arms a 30 s timeout (our
module, `CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT`, see
[BLE policy module](zmk-firmware.md#ble-policy-module)): without a new host,
the keyboard returns to the previously active slot if connected, else the most
recently connected slot, else stays; a pairing in progress (from the passkey
request) pauses it, BT_CLR disarms it. Its event
`rainy75_ble_open_profile_timeout` flashes the empty slot like FAILED.

The runtime setting `ind.passkey_guide` (default on) switches the number-row guidance off
(passkey digits, Enter pulse, verify chase, red digit flash). The digits, the Enter pulse
and the red digit flash are then not drawn, do not turn the effect off and do not keep the
frame loop alive (`guide_running()` in `ble_status.c`). Nothing on the keyboard reacts to a
passkey request until Enter is pressed, so the host's pairing dialog is the only cue that
digits are expected. After Enter the verify chase is still tracked and only its number-row
painting is skipped: the slot being verified keeps blinking, the overlay stays active and
the effect is off until the pairing ends (`rrgb_ble_active()`,
`rrgb_ble_suppress_effect()`). F1..F4 keep the slot status. The events are tracked
meanwhile, so switching the guide on during a pairing shows it at once
(`rrgb_ble_set_passkey_guide()`).

Fn-layer presses leave no reactive trace: while layer 1 is held,
`rrgb_overlay_key_reactive()` is false and `rrgb_on_key()` skips the
ripple/heat and `last_press_tick` for every press, not only F1..F4. All Fn
combinations are commands (BT slots, output, media, RGB controls), and without
this a quickly released Fn left the reactive afterglow on the pressed F-key.

**Effect off during BLE connecting / switching / pairing.** While
`ble_status` shows any automatic animation (no Fn needed), the normal effect is
off and the board is dark except the BLE indicators and the other functional
overlays (CapsLock, Fn-highlight, battery gauge): the switch confirm, the
connected solid + fade, every red flash (lost, failed, cleared, open slot
timeout, also on a background slot), the active slot's fast blink or breathe
only while it is shown (BLE output, 30 s hold window, as gated above), the
passkey guidance and the red digit flash (both only with `ind.passkey_guide` on) and the
verify chase. `rrgb_ble_suppress_effect(tick)` answers this; `rrgb_overlay_suppress_effect()`
adds the BLE build check. The Fn overview alone (including the blink shown
only because Fn is held) does not count. The engine keeps an effect gain
(`rrgb_effect_gain_next()`): down to 0 over 0.1 s
(`RRGB_EFFECT_FADE_OUT_FRAMES`), then the effect is not rendered at all, and
back to full over 0.5 s (`RRGB_EFFECT_FADE_IN_FRAMES`) once no BLE animation
shows. While RGB is off, and when the frame loop stops, the gain snaps to its
target instead (`rrgb_effect_gain_frame()`: 0 while suppressed, else full), so
it never freezes mid fade and RGB toggled on starts at the right level.
Suppression implies `rrgb_overlay_active()`, so the frame loop and the
LED rail stay on through the window also with RGB toggled off. Key presses
while suppressed leave no reactive trace (`rrgb_overlay_key_reactive()` is
false, like with Fn held): the passkey digits typed during pairing would
otherwise bump the heat map, start ripples and step the walker invisibly, and
the reactive effects would pop with that stale state when the effect returns.
The reactive state itself keeps decaying every frame meanwhile (frames run
during the window), so older presses are gone by then too. Host direct mode
(`rgb_mgmt`) is not suppressed: its frame is an explicit host request, not the
normal effect; the BLE indicators still render on top of it.

Render order: `ble_status` is drawn last, so F1..F4 replace the Fn-highlight
white and the passkey guidance wins over the battery gauge on the number row;
it never paints any other key. Key positions (F1..F4 = 1..4, number row
16..25, Enter 43 on ISO / 56 on ANSI) are **keymap-coupled** constants in
`overlay.c` (resolved to LED indices with `rrgb_led_for_position()` at boot).

Inputs (`zmk_adapter.c`, only with `CONFIG_ZMK_BLE`): `zmk_ble_active_profile_changed`,
`zmk_endpoint_changed`, `zmk_ble_auth_state_changed` (patch 0006, queued with
its payload), `rainy75_ble_open_profile_timeout` (our module, queued as FAILED
for the open slot; only with `CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT` > 0), the Bluetooth `connected`/`disconnected` callbacks (a background
slot of a multilink setup changes without a ZMK event), key releases (the output
toggle raises no event while the effective endpoint stays the same) and the
settings commit at boot. Each trigger schedules one work item on the system
workqueue, which applies the queued slot events and then polls slots 0..2
(`zmk_ble_profile_is_open` / `is_connected`, active profile, output = preferred
or selected transport is BLE); a poll without change is a no-op. LOST is
detected in `ble_status` from the polls (CONNECTED -> PAIRED). Events are
stamped with the render frame counter (`rrgb_now()`), which stands still while
the strip is dark, so an animation that starts on a dark strip plays from its
first frame. The work item logs `ble leds: slots a/b/c active n out ble|usb`
on every polled change and `ble leds: <event> slot n digits d` per auth event.

## Idle off / dim and battery cap (runtime settings)

`rgb.idle_s` (0..3600 s, default 0 = never) and `rgb.idle_mode` (`off` default, `dim`):
after that long without a key position event (press or release) the effect turns off
(`off`) or renders at a quarter of its brightness (`dim`); the next key event brings it
back at once. A change of a lighting or indicator setting from a host (`rainy75_cfg.py
set`, the config page) restarts the timer too, so the change shows: the six state-record
settings (`rgb.on`, `rgb.effect`, `rgb.hue`, `rgb.sat`, `rgb.val`, `rgb.speed`),
`rgb.val_battery`, `rgb.idle_*` and `ind.*`; `rgb.cycle` and `rgb.boot_effect` do not. The
functional overlays (CapsLock,
Fn-highlight, battery gauge, BLE status and passkey guidance) keep showing while idle
(the CapsLock tint only with `dim`, it needs the effect);
host direct mode (`rgb_mgmt`) overrides idle off, so a host notification pulse still shows
when the board is idle (that's when you're away). With idle off and no overlay active the
strip is dark and the LED rail is cut after 2 s, as with RGB off. The timer is rainy_rgb's
own (`last_activity_ms` in `engine.c`, decision `rrgb_idle_state()` in `lighting.c`, read
by the render loop every frame). It replaces `CONFIG_RAINY_RGB_IDLE_BLANK` (removed), which
blanked the whole strip, indicators included, after ZMK's activity idle.

`rgb.val_battery` (16..255, default 255 = no cap): while no USB host is connected, the
effect renders at `min(rgb.val, rgb.val_battery)`; the stored `rgb.val` (Fn+↑/↓) does not
change. "USB host connected" is ZMK's `zmk_usb_is_hid_ready()`: a host configured the
keyboard, also while it suspends the bus (PC asleep); pulling the cable goes through a bus
reset, which clears it. This board has no VBUS detection, so `zmk_usb_is_powered()` would
stay true on battery. `zmk_adapter.c` follows the state through
`zmk_usb_conn_state_changed` and hands it to `rrgb_set_usb_host()` in `engine.c`, which
logs `usb host connected` / `usb host gone` on a change. Known limit: if the PC put the USB
bus to sleep and the cable is then pulled, the keyboard can still count as connected to a
USB host (no cap, no low-battery pulse) until the next USB event; with output USB the first
keypress clears it, with output BLE it can persist.

## LED power rail auto-cut (PC2)

Blanking the data alone is not enough on battery: a dark WS2812 still draws
~0.5–1 mA quiescent, ~40–80 mA across the 83 LEDs. Whenever the strip has
stayed dark for **2 s** (RGB toggled off or idle off, with no overlay active),
the render loop drops **PC2** (the MOSFET gate for LED VCC) and
restores it (with a 5 ms settle for WS2812 power-on reset) before the next lit
frame. The 2 s hold-off keeps overlay flicker (CapsLock toggling) from bouncing
the rail. The black frame from `clear_strip()` always goes out while the rail
is still up. Always on (`CONFIG_LED_STRIP_B91_SPI_PC2_POWER`); the other PC2
sites are the led_strip driver init (rail on) and `poweroff.c` (deep sleep).

`zmk_adapter.c` owns the pin: `rrgb_strip_power()` drives it and
`rrgb_strip_rail_state()` samples it, so the register addresses live in one
place. Powering on re-asserts the whole drive configuration (GPIO mode, output
enable, level) rather than just the level, which also makes it the recovery
path for the trap below.

## Rail divergence trap (black box)

`rail_on` in the render loop is loop-local state, so anything that drops PC2
behind the loop's back is invisible to it: every frame then renders into an
unpowered strip, which is silent, logless and dark. This was observed once on
`v0.2.0` (all 83 LEDs dark for hours while the keyboard typed normally, SMP
answered, and a host-mode fill was accepted with zero light), and it self-healed
at a USB resume without the board rebooting.

Every frame the loop samples what it believes and what the pin actually reads,
and records a `B91_DIAG_RGB_STATE` (code 32) event into the USB diagnostic ring
on any change plus a keepalive. The ring is `.noinit` (so it survives a replug
on battery), readable over SMP, and persisted to NVS the moment a divergence is
seen — before the self-heal removes the only symptom:

```bash
reverse/tools/usb_diag.py            # live ring
reverse/tools/usb_diag.py --saved    # the copy persisted at fault time
```

A healthy sample looks like `RGB_STATE rail-believed-on|effect-on|PC2-HIGH|PC2-out-en|PC2-gpio-mode`.
Belief set with any of the three pin bits clear is flagged as a divergence, and
the loop re-asserts the rail (~20 ms) so the board recovers on its own.

The keepalive is **30 min**, paced by the ring rather than by curiosity: 64
entries are shared with the USB events, so a 5 min tick would emit ~96 entries
overnight and wrap away the very divergence the trap exists to catch.

This is instrumentation plus a defensive self-heal, **not a fix** — the cause
has not been identified and the fault has not recurred since the trap went in.

## Config (in `conf/app.conf`)

- `CONFIG_ZMK_RGB_UNDERGLOW=n` (our engine owns the strip)
- `CONFIG_RAINY_RGB=y`, `CONFIG_LED_STRIP_B91_SPI=y`, `CONFIG_LED_STRIP_B91_SPI_PC2_POWER=y`
- `CONFIG_ZMK_HID_INDICATORS=y` (CapsLock). Enabling this changes the USB/BLE HID
  descriptor → **re-plug USB / reconnect BLE once after flashing** so hosts re-read it.
- `CONFIG_NVS`/`CONFIG_SETTINGS_NVS` (persistence), `CONFIG_ZMK_BATTERY_REPORTING` (gauge)
- `CONFIG_RAINY75_CONFIG=y`: the runtime settings of [config-protocol.md](config-protocol.md)
  (`rgb.cycle`, `rgb.val_battery`, `rgb.idle_s`, `rgb.idle_mode`, `ind.*`). Without it the
  engine and the overlay run on the same defaults. `CONFIG_RAINY_RGB_IDLE_BLANK` no longer
  exists (use `rgb.idle_s`).

ZMK is **pinned** in `zmk/west.yml` (not `main`) for reproducibility.

## Build & flash

```
distrobox enter arch -- bash -c "./build.sh"                          # app
distrobox enter arch -- bash -c "./zmk/src/rainy_rgb/tests/run_host_tests.sh"  # host tests
~/go/bin/mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" \
    image upload build/zephyr/zmk.signed.bin                          # then: image test <hash>; reset
```

## Concurrency model

Render thread owns `pixels[]`, the ripple pool, and `key_heat[]`. The ZMK event
thread only **produces** (SPSC press queue append; single-byte/word `volatile`
overlay state writes). `ble_status` setters run in one system-workqueue work
item (slots, output, auth events) and the layer listener (Fn); each variable has
one writer. Single-core RISC-V → benign races by design, no locks.
The host direct-pixel buffer follows the same pattern: the mcumgr (SMP) thread
writes `host_px[]` + a `volatile` flag, the render thread copies it per frame —
a torn write is a one-frame glitch at 50 FPS.

Runtime settings reach the engine, the overlay and `ble_status` as single bytes or
aligned words, written by the mcumgr thread (`set`/`reset`), the ZMK main thread (load at
boot) or init, and read every frame by the render thread; a pair changed together (CapsLock
style and colour, idle seconds and mode) can be torn for one frame. The Fn-highlight key
mask is written by the layer listener (system workqueue) and read by the render thread; a
torn read mixes old and new keys for one frame. The engine reads `rgb.cycle` from the
settings registry (its list copy runs under a spinlock) only on Fn+Enter, on the key event
path (system workqueue), never in the render thread.

## Host control (`rgb_mgmt`, mcumgr group 65)

With `CONFIG_RGB_MGMT=y` a host script can drive the pixels directly over the
same SMP transport as DFU (USB CDC-ACM serial). Group 65, four commands:

| ID | Command | Payload | Effect |
|----|---------|---------|--------|
| 0 | set   | `{"px": bstr}` — quads of `[keymap_position, r, g, b]` | light specific keys; first set after normal mode starts from black; later sets are incremental |
| 1 | fill  | `{"r","g","b"}` | whole board one color |
| 2 | clear | `{}` | exit host mode, back to the normal effect |
| 3 | info  | (read) | `{"n": 83, "host": bool, "beat": uint, "sfree": uint}` |

Positions are **keymap positions** (0..82, row-major), translated through
`led_map` on the device — the same host code works on ISO and ANSI boards.
Host mode is not persisted (reboot/deep sleep return to the normal effect),
functional overlays (CapsLock / Fn-highlight / battery / BLE status) still
render on top, host frames are not blanked during BLE connecting or pairing
(only the normal effect is),
and **any physical Fn+RGB control exits host mode** — a stray script can never
lock the user out of their lighting.

**Host mode also expires on its own** after
`CONFIG_RGB_MGMT_HOST_TIMEOUT_S` (default 30 s) with no `set`/`fill`. Without
that watchdog a host which dies without sending `clear` strands the board on its
last frame forever: idle off cannot rescue it (host mode overrides it by
design) and on battery nothing else intervenes. Undocking mid-animation
is the case that bites: the link dies before the host can retract the frame,
and afterwards there is no host left to send anything. Host animations refresh
continuously (largest gap is well under a second), so the timeout only fires
when the host really is gone. The trade-off is that a deliberately static
"fill and walk away" also reverts after that long; set the option to `0` to
disable and restore indefinite host mode.

`beat` is the **render-loop heartbeat**, advancing once per loop iteration
whether or not a frame is drawn, so a board in idle off still beats (the
private `rt.tick` frame counter deliberately does *not*, and is the wrong thing
to watch here). Sample `info` twice a second apart: if `beat` does not move, the
render thread is dead. That case is worth calling out because every other signal
still looks healthy (the board types, USB and SMP answer, and `set`/`fill` are
*accepted*) while the strip stays dark indefinitely:

```
python3 reverse/tools/rainy75_rgb.py info; sleep 1; python3 reverse/tools/rainy75_rgb.py info
```

A stalled `beat` means the render loop is stuck, not that it faulted. This
firmware overrides Zephyr's fatal-error handler to cold-reboot (see
`zmk/src/mcuboot_confirm.c`), so a thread that actually faults takes the whole
board through MCUboot with it, which is very visible. The silent version is a
stack overflow: the B91 has no PMP stack guard, so the excess frames spill into
the neighbouring thread stack, the loop's saved locals get corrupted, and it
ends up sleeping indefinitely. `CONFIG_STACK_SENTINEL=y` (on in `conf/app.conf`)
catches that at the next context switch and turns it into the logged reboot; a
reboot restarts the thread.

`sfree` is the render thread's **untouched stack bytes**, the high-water mark
the other way round. The 1 KB stack that originally killed this thread was a
guess, and so is the 2 KB replacing it, so this reports the headroom instead of
assuming it: watch it after a long uptime with effects, overlays and host mode
all exercised. It needs `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO` to
paint and walk the stack (both on in `conf/app.conf`), and reads `0` when either
is off.

Host-side client: [`reverse/tools/rainy75_rgb.py`](../reverse/tools/rainy75_rgb.py)
(stdlib-only Python, Linux/macOS):

```
python3 reverse/tools/rainy75_rgb.py set F1 F2 F3 --color ff0000
python3 reverse/tools/rainy75_rgb.py set WASD --color 00ff00   # groups: FROW WASD ARROWS NUMROW ALL
python3 reverse/tools/rainy75_rgb.py fill --color 002040
python3 reverse/tools/rainy75_rgb.py pulse --color ff5f00      # notification pulse, then clear
python3 reverse/tools/rainy75_rgb.py clear
```

### Over Bluetooth

`rgb_mgmt` is also reachable wirelessly — mcumgr group handlers are
transport-agnostic, and the BLE SMP transport
(`CONFIG_MCUMGR_TRANSPORT_BT`) is already enabled as the DFU backup path.
No extra firmware config. Hardware-verified on the Rainy 75 Pro ISO DE
(set / fill / pulse / clear / info over BLE GATT, incl. cross-transport
state consistency with USB).

Client: [`reverse/tools/rainy75_rgb_ble.py`](../reverse/tools/rainy75_rgb_ble.py)
(needs `pip install bleak`; Linux/BlueZ tested, macOS should work via bleak's
CoreBluetooth backend). Same CLI as the serial tool:

```
python3 reverse/tools/rainy75_rgb_ble.py info                  # finds "Rainy 75 Pro" in the BlueZ registry
python3 reverse/tools/rainy75_rgb_ble.py pulse --color ff5f00
python3 reverse/tools/rainy75_rgb_ble.py --address XX:XX:XX:XX:XX:XX set WASD --color 00ff00
```

Notes: the client resolves the keyboard from BlueZ's *known-devices*
registry, not a scan — a connected peripheral doesn't advertise, so
bleak's scan-based lookup can't see it. The link must be encrypted
(Zephyr's SMP BT transport requires it); a keyboard bonded for BLE HID
already satisfies that. Requests larger than one ATT MTU rely on
`CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY=y` (set in `conf/app.conf`).

## Changelog

Notable `rainy_rgb` fixes and features, most recent first; see the linked
sections above for mechanism detail. Releases with no engine changes (v0.2.0,
which shipped USB work only) are omitted.

- **unreleased** (runtime settings): Fn+Enter follows `rgb.cycle`; brightness cap
  without a USB host (`rgb.val_battery`); rainy_rgb's own idle timer with off/dim
  (`rgb.idle_s`, `rgb.idle_mode`), which replaces `CONFIG_RAINY_RGB_IDLE_BLANK` (removed)
  and keeps the indicators visible; CapsLock style and colour (`ind.caps_style`,
  `ind.caps_color`); Fn-highlight keys from the live keymap, so they follow ZMK Studio
  (`ind.fn_highlight`); passkey guide switch (`ind.passkey_guide`); low-battery pulse on
  Esc (`ind.bat_low`). See [config-protocol.md](config-protocol.md).
- **unreleased**: BLE slot status on F1..F4 and passkey guidance on the number
  row (`ble_status`), fed by ZMK patch 0006; verify chase after Enter and red
  digit flash on a wrong code (0006/0007), open slot timeout (module event
  `rainy75_ble_open_profile_timeout`, was patch 0008). See
  [BLE slot status](#ble-slot-status-and-passkey-guidance).
  The normal effect is off (0.1 s fade out, 0.5 s fade in) while any
  automatic BLE animation shows; presses meanwhile leave no reactive trace.

- **v0.2.2** — Root-cause correction for the dark-strip bug (#30): it's a stack
  overflow. The B91 has no PMP stack guard and the BLE RX stack sits directly
  below `rrgb_stack`, so the two threads corrupt each other's frames until the
  render loop sleeps on a garbage deadline. The earlier note blaming Zephyr for
  aborting the render thread alone cannot be right: `mcuboot_confirm.c`
  overrides the fatal handler to cold-reboot, so a real fault takes the whole
  board with it. `CONFIG_STACK_SENTINEL` and `CONFIG_INIT_STACKS` are now set in
  `conf/app.conf`, making the next overflow a logged reboot instead of a silent
  hang (and `sfree` read `0` until `INIT_STACKS` went on).
- **v0.2.2** — Dark-strip fix (#29): render thread stack bumped 1 KB to 2 KB;
  `beat` (render-loop heartbeat) and `sfree` (stack headroom) added to
  `rgb_mgmt` `info`; host mode now auto-expires after
  `CONFIG_RGB_MGMT_HOST_TIMEOUT_S` (default 30 s) of silence, so an undocked
  host can't strand the board on its last frame. See
  [Host control](#host-control-rgb_mgmt-mcumgr-group-65).
- **v0.2.1** — LED rail-divergence trap: every frame samples believed-vs-actual
  PC2 state, records a diagnostic-ring entry on any change (plus a 30 min
  keepalive, so the 64-entry ring can't wrap the fault away), self-heals a
  divergence, and persists the ring to NVS at fault time. Added after a dark
  strip was observed once on v0.2.0, which predates any rail instrumentation;
  nothing has been caught diverging since the trap went in, so that episode
  stays unexplained and separate from the stack-overflow bug above. See
  [Rail divergence trap](#rail-divergence-trap-black-box).
- **v0.1.1** — Activity-idle LED blank (`CONFIG_RAINY_RGB_IDLE_BLANK`) and PC2
  rail auto-cut while the strip stays dark.
- **v0.1.0** — Initial engine: 12 effects, opt-in `walker` calibration
  diagnostic, host-controlled per-key RGB (`rgb_mgmt`, mcumgr group 65) over USB
  and BLE.

## Open items / future

- **Battery accuracy**: validate the ADC (PD1 / 1-2 divider / Vref) against a
  multimeter; optionally read per-chip Vref calibration at flash `0xFE0C0`. The
  gauge degrades gracefully (coarse, on-demand) until then.
- **Per-key / per-layer static color schemes**, Num/Scroll/Compose indicators,
  runtime per-key config (VIA/Studio) — deferred.
- `fx_rain` advances drop state before its NULL-xy guard (cosmetic purity nit);
  SPSC queue overflow is silent (needs 16 presses/33 ms — impossible).
