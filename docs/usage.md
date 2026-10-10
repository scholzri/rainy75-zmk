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
| **Fn + F1 / F2 / F3** | Select **Bluetooth profile 1 / 2 / 3** (also switches from USB to Bluetooth) |
| **Fn + F4** (or Fn + Home) | Toggle output **USB ↔ Bluetooth** |
| **Fn + F5 … F12** | Media: prev · next · mute · vol− · vol+ · play/pause · bright− · bright+ |
| **Fn + Del** | **Remove the selected Bluetooth profile's pairing** (then re-pair) |
| **Fn + Backspace** | RGB on/off |
| **Fn + Enter** | RGB: next effect |
| **Fn + # (key left of Enter)** | RGB: cycle hue |
| **Fn + ↑ / ↓** | RGB: brightness up/down |
| **Fn + ← / →** | RGB: speed down/up |
| **Fn + B** | Battery gauge (~3 s bar on the number row) |

Full RGB details: [rainy-rgb.md](rainy-rgb.md). Every setting (lighting, indicators, Win or
Mac): [config.md](config.md).

## Windows and Mac

The left **Win** and **Alt** keys follow two settings. Change them on the
[config page](config.md) (section Keyboard), or over USB with
`python3 reverse/tools/rainy75_cfg.py set KEY VALUE` (for Bluetooth put `--ble` before
`set`), for example `set kb.os mac`:

| Settings | Left Win key position | Left Alt key position |
|---|---|---|
| `kb.os win` (default) | Win | Alt |
| `kb.os mac` | Option | Command |
| `kb.os win`, `kb.gui_lock on` | nothing | Alt |
| `kb.os mac`, `kb.gui_lock on` | Option | nothing |

With `kb.os mac` the bottom row reads Ctrl, Option, Command, Space, as on a Mac keyboard.
`kb.gui_lock on` silences the GUI key (Win, or Command on a Mac), for example while gaming.
Right Alt stays AltGr. A change applies from the next keypress; a key held while it changes
is released correctly. The settings are stored and survive a restart.

Both keys use the **OS Key** behavior (`&os_key`). ZMK Studio shows it with the choices
"Win (Option on a Mac)" and "Alt (Command on a Mac)". A key you bind to something else in
Studio no longer follows the settings; `python3 reverse/tools/rainy75_cfg.py get kb.os_keys`
shows how many keys use the OS Key behavior (2 with the default keymap; the config page shows
the number and warns at 0). To get the behavior back on a key you rebound, choose "OS Key"
for it again in Studio and save; "Restore Stock Settings" in Studio restores both keys too,
but it also drops every other edit you saved in Studio.

## Bluetooth and USB

The keyboard remembers **three Bluetooth hosts** (profiles 1, 2 and 3 on F1, F2 and F3)
plus USB. Up to three hosts can stay connected at the same time; the keys go to the one
you selected.

### Quick reference

| I want to... | Do this |
|---|---|
| Pair a new host | `Fn + F1/F2/F3` on a free profile, pair "Rainy 75 Pro" on the host, type the code it shows, `Enter` |
| Switch to another Bluetooth host | `Fn + F1 / F2 / F3` |
| Switch to USB (cable plugged in) | `Fn + F4` (toggles USB and Bluetooth) |
| Back to Bluetooth from USB | `Fn + F1/F2/F3` (switches to Bluetooth by itself) or `Fn + F4` |
| Remove a pairing | `Fn + F1/F2/F3` to select it, then `Fn + Del`, and remove the keyboard on the host too |
| See which profiles are paired | Hold `Fn` and look at F1 to F4 |

Make sure the wireless switch **under the CapsLock keycap** is on.

### Pair a new host

1. Press `Fn + F1`, `Fn + F2` or `Fn + F3` to select a **free** profile. Its F-key
   blinks blue fast: the keyboard is waiting for a new host.
2. On the host, open the Bluetooth settings and pick **"Rainy 75 Pro"**.
3. The host shows a **6-digit code**. The number row lights up dim white on the keyboard.
   Type the code on the number row (keys 1 to 6 turn blue as you type) and press **Enter**.
   The digits are not typed into any computer.
4. Keys 1 to 6 run a blue chase while the host checks the code. When it is accepted the
   F-key stays blue for 2 seconds and fades: done.

If you mistype, just type all six digits again (the last six count) before Enter. `Esc`
cancels. A wrong code flashes keys 1 to 6 and the F-key red; start again from the host.

If no host pairs within **30 seconds**, the keyboard gives up: the free profile flashes red
and the keyboard returns to the host you used before (or the last one that was connected).

The profile must be free. To pair a host on a profile that is already taken, remove that
pairing first (below).

### Switch hosts, or between USB and Bluetooth

- `Fn + F1 / F2 / F3` selects a Bluetooth host. Its F-key lights blue briefly. If that host
  is not connected yet, the key breathes slowly until it connects (after a few seconds,
  or once the host wakes up).
- `Fn + F4` toggles between **USB** and **Bluetooth**. Selecting a Bluetooth profile while
  on USB switches to Bluetooth by itself.
- With the cable unplugged the keyboard always types over Bluetooth.

While the keyboard connects, switches or pairs, the normal lighting effect turns off so the
status lights are easy to see; it comes back by itself.

### Remove a pairing

1. `Fn + F1/F2/F3` to select the profile you want to free.
2. `Fn + Del`. The F-key flashes red, then blinks fast: the profile is free and ready to pair.
3. On the host, **remove / forget "Rainy 75 Pro"** in the Bluetooth settings. Without this
   step the host keeps old keys and cannot pair again.

Repeat for each profile to remove all of them.

### What the F1 to F4 lights mean

Hold **Fn** for an overview:

| Key | Colour | Meaning |
|---|---|---|
| F1 to F3 | bright blue | selected profile, connected |
| F1 to F3 | bright blue, blinking fast | selected profile, free, waiting for a new host |
| F1 to F3 | bright blue, breathing | selected profile, paired, connecting |
| F1 to F3 | dim blue | another host, connected in the background |
| F1 to F3 | very dim blue | paired, not connected |
| F1 to F3 | very dim white | free profile |
| F4 | white | typing over USB |
| F4 | cyan | typing over Bluetooth |

Without Fn, the F-keys only light up when something happens: blinking or breathing while
pairing or connecting (for up to 30 seconds), blue then fade when a host connects, and three
red flashes when a connection is lost, a pairing fails or a pairing is removed. The full
LED reference is in [rainy-rgb.md](rainy-rgb.md#ble-slot-status-and-passkey-guidance).

### Waking up

After 15 minutes without a keypress the keyboard sleeps. Press any key to wake it; it
reconnects to the selected host within a few seconds (about 5 s measured).

## ZMK Studio (live keymap editing)

[ZMK Studio](https://zmk.studio) lets you edit the keymap live, without reflashing. It works
**over USB and over Bluetooth**, in **Chrome or Edge** (Firefox supports neither Web Serial
nor Web Bluetooth).

Studio answers over both, whatever the output is set to (F4). Switching from a Bluetooth
client to a USB client or back locks Studio again: press `Fn + ESC` to unlock.

**Over USB:**
1. Open **[zmk.studio](https://zmk.studio)**, choose **Connect → USB**.
2. The keyboard has two serial ports. Pick the **Studio** one: with the included udev rules
   (`99-rainy75-zmk.rules`) Linux shows it as **"Rainy 75 Pro Studio"**; without them, or on
   other systems, it's the second Rainy port (USB interface 3, usually the higher port
   number). On Windows both show as "USB Serial Device (COMx)"; Device Manager → Properties
   → Details → Hardware IDs shows `MI_03` for the Studio port and `MI_00` for the other one.
   The other port is the log console and firmware updates; picking it just doesn't connect.
3. Press **`Fn + ESC`** on the keyboard to **unlock** editing.

Over USB Studio is fast: about 300 requests per second (about 25 over Bluetooth).

**Over Bluetooth:**
1. Open **[zmk.studio](https://zmk.studio)**, choose **Connect → Bluetooth**, and pick the
   keyboard in the browser's device picker.
2. Press **`Fn + ESC`** on the keyboard to **unlock** editing.

> **"Failed to open the serial port"** on Linux with the udev rules installed usually means a
> security policy blocks the browser, not the keyboard. On Fedora secureblue that's SELinux
> in enforcing mode; Studio over Bluetooth works without changing it.

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

**Update the firmware (mcumgr DFU).** Once you're on ZMK, flash a new build over USB, no
debugger, no bootloader button. The included tool finds the keyboard by itself and takes
about 12 s (Python 3, no extra packages):

```bash
python3 reverse/tools/rainy75_dfu.py upload build/zephyr/zmk.signed.bin --test --reset
```

It uploads the image, checks it arrived intact, marks it for a test boot and resets.
`rainy75_dfu.py list` shows the images; `confirm` makes the running one permanent by hand.

The [mcumgr CLI](https://github.com/apache/mynewt-mcumgr-cli) works too, but takes about 85 s
over USB (it pauses after every serial line, which USB does not need):

```bash
M='mcumgr --conntype serial --connstring dev=/dev/ttyACM0,baud=115200'
$M image upload build/zephyr/zmk.signed.bin   # ~85 s
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
