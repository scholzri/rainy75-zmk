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
| `engine.{h,c}` | Owns `pixels[83]` + a dedicated **50 FPS render thread**; runtime state; the FPS-independent speed model; settings load; dispatch (effect → overlay → strip). |
| `reactive.{h,c}` | Lock-free **SPSC press queue** (event thread → render thread) feeding an 8-slot ripple pool + per-LED `key_heat[83]`. |
| `overlay.{h,c}` | Functional indicators (CapsLock / Fn-highlight / battery gauge). Pure, ZMK-free. |
| `led_map.{h,c}` | Calibrated `pos_to_led[83]` + `led_positions[83]` (XY) + lookups. ISO and ANSI table variants (`CONFIG_RAINY_RGB_ANSI_LEDMAP`, set by `./build.sh --ansi`). |
| `state.c` | NVS persistence (`SETTINGS_STATIC_HANDLER`, subtree `rainy_rgb/`, 2 s debounce). |
| `zmk_adapter.{h,c}` | **ZMK boundary**: led_strip wrap + `ZMK_LISTENER`/`ZMK_SUBSCRIPTION` for position/layer/hid-indicators/battery → neutral setters. |
| `../behaviors/behavior_rainy_rgb.c` | **ZMK boundary**: the `&rgb` keymap behavior → engine API. |
| `tests/test_{color,effects,overlay}.c` | Host gcc unit tests (run `tests/run_host_tests.sh`). |

The board DTS exposes the strip as `chosen zmk,underglow = &led_strip` (driver
`telink,b91-spi-led-strip`, PB7 MOSI, DMA ch4, ~6 MHz, GRB, PC2 = LED VCC MOSFET).

## Render pipeline (per frame, render thread only)

```
reactive_tick (drain key presses → ripples + heat)
  → effect renders into pixels[]  (or black base if RGB toggled off)
  → overlay_render  (Fn-highlight base-override, then CapsLock, then battery gauge)
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

## Controls (Fn layer)

| Combo | Action |
|-------|--------|
| Fn+Backspace | RGB toggle (on/off) |
| Fn+Enter | next effect |
| Fn+# (NUHS) | hue |
| Fn+↑ / Fn+↓ | brightness |
| Fn+→ / Fn+← | speed |
| Fn+B | battery gauge (~3 s) |

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

- **CapsLock** → the CapsLock key glows **white** (`hid_indicators_changed`, bit 1).
  Requires `CONFIG_ZMK_HID_INDICATORS=y`. Over BLE, some hosts never send the LED
  report, so caps may not update on BLE.
- **Fn-highlight** → while Fn (layer 1) is held, only keys with an Fn binding light
  white, rest dark (`zmk_keymap_layer_active(1)`). The whole top row lights because
  every top-row key is Fn-mapped (Studio/BT/output/media). Position set is
  hardcoded in `overlay.c` `fn_keys[]` — **keymap-coupled**, update if the Fn layer changes.
- **Battery gauge** (Fn+B) → a 10-segment bar on the number row, level-colored
  (green→red), ~3 s. **Approximate** — the battery-ADC pin/divider/Vref are not yet
  hardware-validated (see Open items).

## Activity-idle blank (opt-in)

With `CONFIG_RAINY_RGB_IDLE_BLANK=y` the strip turns **off after
`CONFIG_ZMK_IDLE_TIMEOUT` of no activity** (typing / pointing) and comes back on
the first keypress — while the board is still awake, before deep sleep. On
wireless this saves the per-key LED current during idle-but-awake stretches
(deep sleep already blanks everything; this covers the gap before it). The
engine subscribes to ZMK's `activity_state_changed` event and gates the render
loop on `rt.idle`. Host direct mode (`rgb_mgmt`) overrides the blank, so a host
notification pulse still shows when the board is idle (that's when you're away).
Off by default — `IS_ENABLED()` folds it out with zero behaviour change.

## LED power rail auto-cut (PC2)

Blanking the data alone is not enough on battery: a dark WS2812 still draws
~0.5–1 mA quiescent, ~40–80 mA across the 83 LEDs. Whenever the strip has
stayed dark for **2 s** (RGB toggled off with no overlay, or the activity-idle
blank above), the render loop drops **PC2** — the MOSFET gate for LED VCC — and
restores it (with a 5 ms settle for WS2812 power-on reset) before the next lit
frame. The 2 s hold-off keeps overlay flicker (CapsLock toggling) from bouncing
the rail. The black frame from `clear_strip()` always goes out while the rail
is still up. Always on (`CONFIG_LED_STRIP_B91_SPI_PC2_POWER`); the other PC2
sites are the led_strip driver init (rail on) and `poweroff.c` (deep sleep).

## Config (in `conf/app.conf`)

- `CONFIG_ZMK_RGB_UNDERGLOW=n` (our engine owns the strip)
- `CONFIG_RAINY_RGB=y`, `CONFIG_LED_STRIP_B91_SPI=y`, `CONFIG_LED_STRIP_B91_SPI_PC2_POWER=y`
- `CONFIG_ZMK_HID_INDICATORS=y` (CapsLock). Enabling this changes the USB/BLE HID
  descriptor → **re-plug USB / reconnect BLE once after flashing** so hosts re-read it.
- `CONFIG_NVS`/`CONFIG_SETTINGS_NVS` (persistence), `CONFIG_ZMK_BATTERY_REPORTING` (gauge)

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
overlay state writes). Single-core RISC-V → benign races by design, no locks.
The host direct-pixel buffer follows the same pattern: the mcumgr (SMP) thread
writes `host_px[]` + a `volatile` flag, the render thread copies it per frame —
a torn write is a one-frame glitch at 50 FPS.

## Host control (`rgb_mgmt`, mcumgr group 65)

With `CONFIG_RGB_MGMT=y` a host script can drive the pixels directly over the
same SMP transport as DFU (USB CDC-ACM serial). Group 65, four commands:

| ID | Command | Payload | Effect |
|----|---------|---------|--------|
| 0 | set   | `{"px": bstr}` — quads of `[keymap_position, r, g, b]` | light specific keys; first set after normal mode starts from black; later sets are incremental |
| 1 | fill  | `{"r","g","b"}` | whole board one color |
| 2 | clear | `{}` | exit host mode, back to the normal effect |
| 3 | info  | (read) | `{"n": 83, "host": bool}` |

Positions are **keymap positions** (0..82, row-major), translated through
`led_map` on the device — the same host code works on ISO and ANSI boards.
Host mode is not persisted (reboot/deep sleep return to the normal effect),
functional overlays (CapsLock / Fn-highlight / battery) still render on top,
and **any physical Fn+RGB control exits host mode** — a stray script can never
lock the user out of their lighting.

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

## Open items / future

- **Battery accuracy**: validate the ADC (PD1 / 1-2 divider / Vref) against a
  multimeter; optionally read per-chip Vref calibration at flash `0xFE0C0`. The
  gauge degrades gracefully (coarse, on-demand) until then.
- **Per-key / per-layer static color schemes**, Num/Scroll/Compose indicators,
  runtime per-key config (VIA/Studio) — deferred.
- `fx_rain` advances drop state before its NULL-xy guard (cosmetic purity nit);
  SPSC queue overflow is silent (needs 16 presses/33 ms — impossible).
