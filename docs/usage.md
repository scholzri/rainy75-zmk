# Using the keyboard

Controls for **this ZMK firmware**. (The Fn-combo tables in the reverse-engineering docs —
`gpio-matrix.md`, `architecture.md` — describe the **stock** firmware and do **not** apply
here.) The keymap source is [`zmk/boards/rainy75/rainy75.keymap`](../zmk/boards/rainy75/rainy75.keymap).

The **Fn** key is on the bottom row (between Right-Alt and Right-Ctrl). Hold it for the
combos below.

## Fn-layer reference

| Combo | Action |
|-------|--------|
| **Fn + ESC** | ZMK Studio **unlock** (allow live keymap editing) |
| **Fn + F1 / F2 / F3** | Select **Bluetooth profile 1 / 2 / 3** |
| **Fn + F4** (or Fn + Home) | Toggle output **USB ↔ Bluetooth** |
| **Fn + F5 … F12** | Media: prev · next · mute · vol− · vol+ · play/pause · bright− · bright+ |
| **Fn + Del** | **Clear the current BT profile's bond** (then re-pair) |
| **Fn + Backspace** | RGB on/off |
| **Fn + Enter** | RGB: next effect |
| **Fn + # (key left of Enter)** | RGB: cycle hue |
| **Fn + ↑ / ↓** | RGB: brightness up/down |
| **Fn + ← / →** | RGB: speed down/up |
| **Fn + B** | Battery gauge (~3 s bar on the number row) |

Full RGB details: [rainy-rgb.md](rainy-rgb.md).

## Bluetooth

This firmware keeps **three independent BLE profiles** (so you can pair three hosts and
switch between them).

- **Switch host:** `Fn + F1 / F2 / F3`.
- **Pair a new host:** select a free profile (`Fn + F1/F2/F3`) — the keyboard advertises as
  **"Rainy 75 Pro"** — then pair it from the host's Bluetooth settings.
- **Wired vs wireless:** `Fn + F4` toggles the output between USB and BLE.
- **Reset / re-pair a profile:** on the active profile, press **`Fn + Del`** — it clears
  that bond and starts advertising again, so you can pair fresh. (Clear all three by doing
  `Fn+F1 → Fn+Del`, `Fn+F2 → Fn+Del`, `Fn+F3 → Fn+Del`.)
- **Radio on/off:** there's a physical wireless switch **under the CapsLock keycap**.

> First connection after a cold boot can fail once and succeed on retry (a quirk of the
> Telink BLE controller) — just reconnect.

## ZMK Studio (live keymap editing)

[ZMK Studio](https://zmk.studio) lets you edit the keymap live, without reflashing. This
firmware exposes Studio **over Bluetooth only**, so that **mcumgr DFU stays available over
USB** — on this MCU you can't have both, and keeping USB firmware updates wins.

Why: the B91 has just **256 B of USB SRAM** shared across all data endpoints, and a single
USB serial port (CDC-ACM) already costs ~136 B of it. That leaves room for **exactly one**
CDC-ACM, which this firmware spends on the **serial console *and* mcumgr DFU** (they share
the one port). USB Studio would need a *second* CDC-ACM — that overflows the 256 B SRAM, an
endpoint fails to allocate, and the **whole USB device stops enumerating**: no console, no
DFU, *and* no Studio. The only way back from that is a hardware reflash with the Telink EVK.
So Studio runs over BLE, which costs nothing from the USB budget.

1. Open **[zmk.studio](https://zmk.studio)** in **Chrome or Edge** (Web Bluetooth — Firefox
   is not supported).
2. **Connect → Bluetooth**, and pick the keyboard in the browser's device picker.
3. Press **`Fn + ESC`** on the keyboard to **unlock** editing.

### Troubleshooting: "No Services matching UUID … found in Device"

That error means the host is showing a **stale Bluetooth GATT cache** (it remembers the
keyboard from before this firmware, without the Studio service). Clear it on both sides:

1. **Keyboard:** `Fn + Del` on the active profile (clears its bond).
2. **OS:** remove / forget the keyboard in your Bluetooth settings.
3. **Chrome:** open `chrome://bluetooth-internals/` → **Devices** → find the keyboard →
   **Forget**.
4. **Re-pair** the keyboard, then **Connect** again in ZMK Studio.

Make sure the wireless switch (under CapsLock) is **on** and the output is BLE (`Fn + F4`).

## Serial console & firmware updates (over USB)

Both use the keyboard's USB serial port — `/dev/ttyACM0` on Linux — at **115200 baud**.

**View the log (serial console).** ZMK prints boot and runtime logs there; handy for
debugging BLE / RGB / boot issues:

```bash
screen /dev/ttyACM0 115200        # exit: Ctrl-A then K
# or: minicom -D /dev/ttyACM0 -b 115200   /   cat /dev/ttyACM0
```

**Update the firmware (mcumgr DFU).** Once you're on ZMK, flash a new build over USB with
[mcumgr](https://github.com/apache/mynewt-mcumgr-cli) — no debugger, no bootloader button:

```bash
M='mcumgr --conntype serial --connstring dev=/dev/ttyACM0,baud=115200'
$M image upload build/zephyr/zmk.signed.bin   # ~80 s
$M image list                                 # note the slot-1 hash
$M image test <hash>                          # mark it for swap
$M reset                                      # MCUboot swaps on reboot
```

MCUboot swaps the image on reset; if the new one fails to boot, the watchdog reverts to the
old image automatically. Build the image first — see
[INSTALL.md](../INSTALL.md#4-build-from-source).

> The console and mcumgr share the single CDC-ACM port, so **close the serial console
> before running mcumgr** (otherwise the port is busy). The first mcumgr command right after
> a fresh boot can time out — just retry.
