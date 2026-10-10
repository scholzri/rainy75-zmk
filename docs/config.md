# Settings: the config page and the command line

The keyboard keeps its lighting, indicator and keyboard settings on the keyboard itself.
You change them from a web page or from a command-line tool, over USB or Bluetooth; every
change applies at once and is stored (about 2 s after the last change), so it survives a
restart. The Fn keys keep working and the page shows what they change. The keymap itself
stays with [ZMK Studio](usage.md#zmk-studio-live-keymap-editing); the page covers what Studio cannot show.

This needs a firmware with the runtime settings (v0.4.0 or later). An older firmware
answers "This firmware has no runtime settings": update it first ([INSTALL.md](../INSTALL.md);
on a keyboard that already runs ZMK: [usage.md](usage.md#serial-console--firmware-updates-over-usb)).

## The page

**[scholzri.github.io/rainy75-zmk/config/](https://scholzri.github.io/rainy75-zmk/config/)**

- Browsers: **Chrome or Edge** on Windows, macOS, Linux or ChromeOS (they have Web Serial
  for USB and Web Bluetooth). Firefox and Safari have neither; the page says so and offers
  the demo. On Linux, Bluetooth in Chrome needs `chrome://flags/#enable-web-bluetooth`
  turned on (then restart Chrome); without it the Connect Bluetooth button stays greyed out
  and a note says why, and USB still works.
- **Demo:** [the same page with `?demo`](https://scholzri.github.io/rainy75-zmk/config/?demo)
  runs against a simulated keyboard, in any browser. Nothing is sent anywhere. Below the
  settings it has two buttons that act like the keyboard: "Press Fn+Enter" (the next effect
  of the effect cycle) and "Rebind the Win and Alt keys in ZMK Studio" (shows the warning
  of the Keyboard section).
- **Privacy:** the page is one file that loads nothing (no fonts, scripts or trackers; its
  Content Security Policy blocks every network request). It talks only to the keyboard
  you pick. The browser remembers which port or keyboard you allowed, nothing else.
- **The official copies** are the address above and the file `rainy75-config.html`
  attached to each [release](https://github.com/scholzri/rainy75-zmk/releases). The page
  never offers a download or asks you to install anything (see [SECURITY.md](../SECURITY.md)).

### Over USB

1. Plug in the cable and click **Connect USB**.
2. Chrome lists the keyboard twice, because it has two serial ports: the console (log,
   firmware updates and these settings) and the ZMK Studio port. With the udev rules from
   [INSTALL.md](../INSTALL.md) Linux names them "Rainy 75 Pro Console" and "Rainy 75 Pro
   Studio"; elsewhere they look the same. Pick the console if you can tell them apart,
   otherwise either: if you picked the Studio port, the page notices within a second ("This
   is the ZMK Studio port. Choose the other Rainy 75 port (the console).") and opens the
   list again. If the browser wants a fresh click for that, the page asks you to click
   Connect USB again.
3. Next time the page connects by itself, without the list, as long as the keyboard is
   plugged in (also when you plug it in while the page is open). After you click
   Disconnect, click Connect USB again.

The page holds the console port while it is connected: close it (or click Disconnect)
before you use `rainy75_cfg.py`, a serial terminal or a firmware update tool.

ZMK Studio and the page can be open at the same time: Studio uses the Studio port, the page
the console port. Only one program can hold a port: if the page says "The port could not be
opened", close the other program (a serial terminal, `rainy75_cfg.py`, a second tab of the
page); if you picked the Studio port while Studio is connected over USB, that is the
reason, so pick the console port. On Linux without the udev rules, Chrome may not be
allowed to open the port at all; install them as [INSTALL.md](../INSTALL.md) describes.

Do not use ZMK Studio over Bluetooth while the page connects over USB. To find the console
port the page sends a short test request to the ports it checks, the Studio port among
them, and Studio requests that arrive over Bluetooth during that probe can be dropped.
Connect the page first, then open Studio.

### Over Bluetooth

1. The keyboard must be **paired with this computer**. The settings need that pairing (an
   authenticated bond); a keyboard paired only with another computer gets the message
   "The keyboard is not paired with this computer". To pair: select a free profile with
   Fn+F1, F2 or F3, pick "Rainy 75 Pro" in the computer's Bluetooth settings, and type the
   code the computer shows on the keyboard's number row, then press Enter
   ([usage.md](usage.md#pair-a-new-host)). If the computer lists the keyboard as paired but
   the page still says it is not paired, remove the pairing on both sides first
   ([usage.md](usage.md#remove-a-pairing)).
2. Click **Connect Bluetooth** and choose "Rainy 75 Pro".
3. Next time the page connects without the list where the browser supports it (in Chrome
   behind `chrome://flags/#enable-experimental-web-platform-features`); otherwise the list
   opens again. If the remembered keyboard does not answer, the page says so and the next
   click on Connect Bluetooth opens the list.

When the keyboard is idle it listens only to a fraction of the Bluetooth connection events
to save power, so a change can take up to about half a second to arrive. The page therefore
sends slider moves at most every 150 ms over Bluetooth instead of every 50 ms over USB.

### What the page shows

Three sections, **Lighting**, **Indicators** and **Keyboard**, with a switch, slider,
choice, colour picker or list per setting and a line of help under each. The top bar shows
the connection and the firmware version ("test image (not confirmed)" for a test image).
The page asks the keyboard every second whether something changed (for example with the Fn
keys) and shows the new values; it pauses while the tab is in the background and catches
up when you come back. If the keyboard refuses a change, the page says why and shows the
value the keyboard kept. **Reset to defaults** sets every setting back (after a question).
If the cable is pulled or Bluetooth drops, the page says "Connection lost" and offers the
connect buttons again; over USB it also connects by itself when the keyboard is plugged
back in, and the message goes away.

The number of keys that follow the Computer and GUI key lock settings changes with ZMK
Studio edits, which do not count as a settings change; the page reads it again every few
seconds and whenever the page gets the focus again, so a Studio edit shows within a few
seconds, or at once when you switch back to the page.

"N settings need a newer page" means the firmware has settings this copy of the page does
not know yet: use the page at the address above, or the copy from the release that matches
your firmware. A choice whose current value this page does not know shows "Other (needs a
newer page)".

Messages you may meet:

| Message | What it means |
|---|---|
| This browser cannot connect to the keyboard | Neither Web Serial nor Web Bluetooth: use Chrome or Edge on a computer, or the demo. |
| USB needs Chrome or Edge on a desktop computer. / Bluetooth is not available in this browser. | The other connection still works. On Linux, Bluetooth needs the Chrome flag above. |
| This is the ZMK Studio port. | You picked the Studio port: pick the other one. |
| The port could not be opened (...) | Another program holds the port: close it and try again. While ZMK Studio is connected over USB its port is busy: choose the other Rainy 75 port. |
| The keyboard is not paired with this computer. | Pair it first, as under Over Bluetooth: a free profile (Fn+F1, F2 or F3), the computer's Bluetooth settings, the code typed on the keyboard's number row, Enter. If the computer already lists it as paired, remove the pairing on both sides and pair again. |
| Bluetooth adapter not available. | The computer has no working Bluetooth adapter (on some systems also when Bluetooth is switched off; Chrome may instead show its own "Bluetooth is off" note in the chooser). Turn Bluetooth on or use USB. |
| The keyboard did not answer over Bluetooth. | The remembered keyboard is not reachable: click Connect Bluetooth again and choose it from the list. |
| This firmware has no runtime settings | The firmware is older than v0.4.0: update it. |
| The keyboard refused: invalid value (rc 3) | The keyboard did not accept the value (also "unknown setting", "read-only setting"). The page shows the value the keyboard has. |
| Connection lost. Connect again. | The cable was pulled, Bluetooth dropped, or the keyboard restarted or went to sleep. |

## Offline

The page needs no internet. Save `rainy75-config.html` from a release (or use
`web/config/index.html` from a clone of the repository) and either

- serve it from your own computer, an address Chrome treats like the hosted page:

  ```bash
  cd web            # or the folder with rainy75-config.html
  python3 -m http.server 8000
  ```

  and open `http://localhost:8000/config/` (or `http://localhost:8000/rainy75-config.html`),
- or open the file directly. Chrome treats a file opened from disk as a secure page too:
  the port and device lists open, and a port or keyboard you allowed before is picked up
  again after a reload without the list (tested with Chromium 154 on Linux over USB and
  Bluetooth). If your browser behaves differently, use the local server above.

The demo works offline too (`index.html?demo`).

## The command line

`reverse/tools/rainy75_cfg.py` does the same from a terminal, from a clone of the
repository (Python 3 on Linux or macOS, no packages for USB; `--ble` needs `bleak` and a
keyboard paired with this computer, as under Over Bluetooth):

```bash
python3 reverse/tools/rainy75_cfg.py info               # protocol, effects, change counter
python3 reverse/tools/rainy75_cfg.py list               # every setting with its range
python3 reverse/tools/rainy75_cfg.py get                # all values (or: get rgb.val kb.os)
python3 reverse/tools/rainy75_cfg.py set rgb.effect plasma
python3 reverse/tools/rainy75_cfg.py set rgb.cycle solid,plasma,wave
python3 reverse/tools/rainy75_cfg.py set ind.caps_color FF8000
python3 reverse/tools/rainy75_cfg.py reset              # all settings (or: reset kb.os)
python3 reverse/tools/rainy75_cfg.py --ble get          # over Bluetooth
```

Values: `on`/`off` for switches, numbers, names for choices, colours as six hex digits
`RRGGBB` (`FF8000`, `0xFF8000` or a quoted `'#FF8000'`), lists as names separated by
commas (`set rgb.cycle ''` is the empty list, which means all effects). The port is found
by name; `--port` (or the environment variable `RAINY75_PORT`) names it, and `--address`
names the Bluetooth address. The protocol is in [config-protocol.md](config-protocol.md).

## Every setting

The page label is in brackets. Defaults reproduce the behaviour before the settings
existed.

### Lighting

| Key | Values | Default | What it does |
|---|---|---|---|
| `rgb.on` (Lighting on) | on, off | on | The effect on or off (Fn+Backspace). The CapsLock key, the Fn highlight, the battery gauge and the Bluetooth status still show when it is off; the CapsLock tint and the low-battery pulse do not. |
| `rgb.effect` (Effect) | an effect | `solid` | The lighting effect (Fn+Enter steps to the next one of the effect cycle). |
| `rgb.hue` (Hue) | 0 to 255 | 0 | The colour of the effect, once around the colour wheel (Fn + the # key left of Enter). |
| `rgb.sat` (Saturation) | 0 to 255 | 255 | 0 is white, 255 the full colour. |
| `rgb.val` (Brightness) | 16 to 255 | 200 | Brightness of the effect (Fn+Up, Fn+Down). |
| `rgb.speed` (Speed) | 1 to 255 | 32 | Animation speed (Fn+Right, Fn+Left). |
| `rgb.cycle` (Effect cycle) | up to 16 effects, in order | all effects | The effects Fn+Enter steps through: the next entry after the current effect, or the first entry when the current effect is not in the list. On the page: tick the effects and drag them (or use the up and down arrow buttons, "Move up" and "Move down", of a row) into order; at least one stays ticked. An empty list, which only the command line can set, means all effects, and the page shows it as all effects ticked. Applies from the next Fn+Enter. |
| `rgb.boot_effect` (Effect at power-on) | `last` or an effect | `last` | The effect after power-on; `last` ("Last used") keeps the effect you chose last. Applies from the next start. |
| `rgb.val_battery` (Brightness cap without USB) | 16 to 255 | 255 (no cap) | While no USB host is connected, the effect is drawn at most this bright; `rgb.val` does not change. |
| `rgb.idle_s` (Idle after) | 0 to 3600 s | 0 (never) | Seconds without a key press until the effect turns off or dims; the next key brings it back. The CapsLock key, the Fn highlight, the battery gauge and the Bluetooth status keep showing. |
| `rgb.idle_mode` (When idle) | `off`, `dim` | `off` | Off, or a quarter of the brightness. With `off` the CapsLock tint and the low-battery pulse go dark too, because they need the effect. |

Effects (the page capitalizes the names and writes `speedcolour` as "Speed colour"):
`solid` (one colour on every key), `rainbow` (a rainbow moving along the keys), `plasma`
(flowing colour waves), `twinkle` (keys sparkle at random), `comet` (a bright dot with a
fading tail runs along the keys), `aurora` (a slow drift between two neighbouring colours),
`reactive` (the whole board flashes on every key press), `ripple` (rainbow rings spread from
each key you press), `wave` (a diagonal wave moves across the board), `rain` (drops fall from
the top row to the bottom), `heatmap` (keys glow when pressed and cool down over time),
`speedcolour` (the colour deepens the faster you type), and in builds with
`CONFIG_RAINY_RGB_WALKER` the LED calibration aid `walker` ("Walker (diagnostic)": lights
one LED and steps to the next on each key press). Details: [rainy-rgb.md](rainy-rgb.md).

"No USB host connected" means no computer has set up the keyboard over USB; a computer
that sleeps still counts as connected, except while the output is Bluetooth: then a USB bus
the computer put to sleep counts as no host (the board dims while the computer sleeps). The
keyboard cannot sense the cable itself, and a pull can look like a computer going to sleep:
with the output on Bluetooth the cap and the low-battery pulse apply within about a second
of the pull, with the output on USB from the next key press (a few seconds after the pull).

### Indicators

| Key | Values | Default | What it does |
|---|---|---|---|
| `ind.caps_style` (CapsLock indicator) | `key`, `tint`, `off` | `key` | With CapsLock on: `key` lights the CapsLock key in the colour below at full strength, `tint` mixes every key 50/50 with it, `off` shows nothing. The tint is as bright as the effect (it follows Brightness, the cap without USB and Dim) and shows only while the effect is drawn: not with the lighting off or idle `off`, not during a Bluetooth animation and not while Fn is held with the Fn highlight on. |
| `ind.caps_color` (CapsLock colour) | colour | white (`FFFFFF`) | The colour of the CapsLock indicator. |
| `ind.fn_highlight` (Fn highlight) | on, off | on | While Fn is held, keys with an Fn function light white and the others go dark. Follows keymap changes made in ZMK Studio. Off leaves the lighting as it is (F1 to F4 still show the Bluetooth slots). |
| `ind.passkey_guide` (Pairing code guide) | on, off | on | Number-row guidance while you type a Bluetooth pairing code ([usage.md](usage.md#pair-a-new-host)). Off: nothing on the keyboard shows that a code is expected; the pairing dialog on the computer is the only cue. The F1 to F4 status stays. |
| `ind.bat_low` (Low battery warning) | 0 to 50 % | 0 (off) | While no USB host is connected and the battery is below this level, Esc pulses red. It shows only while the effect is drawn: with the lighting off or idle `off`, check the battery with Fn+B (the gauge shows for 3 s). |

### Keyboard

| Key | Values | Default | What it does |
|---|---|---|---|
| `kb.os` (Computer) | `win`, `mac` | `win` | `mac` swaps the left Win and Alt keys to Option and Command, as on a Mac keyboard ([usage.md](usage.md#windows-and-mac)). Right Alt stays AltGr. Applies from the next key press. |
| `kb.gui_lock` (GUI key lock) | on, off | off | Silences the GUI key (Win, or Command on a Mac), for example while gaming. |
| `kb.os_keys` (Keys following these settings) | 0 to 83, read-only | counted (2 with the default keymap) | How many keys use the OS Key behavior, counted from the live keymap. 0 means the left Win and Alt keys were rebound in ZMK Studio, so `kb.os` and `kb.gui_lock` change nothing; the page shows a warning then. Bind them to "OS Key" in Studio again and save, or use Restore Stock Settings there (it also drops your other saved Studio edits). |
| `kb.sleep_min` (Sleep after) | 0 to 120 min | 15 | Minutes without a key press until the keyboard sleeps; 0 = never. A settings change (from the page, `rainy75_cfg.py` or the Fn keys) counts as a key press; an open page alone does not keep it awake. Not while a USB host is connected, unless `kb.sleep_on_usb` is on. Any key wakes it; typing works again after about 5 s. |
| `kb.sleep_on_usb` (Sleep on USB) | on, off | off | Also sleep while a USB host is connected. Off: on USB the keyboard stays awake, also while the computer sleeps, so a key press can wake the computer; but it still sleeps while it types over Bluetooth to another device and the computer has had USB asleep for about a minute, because a key press then goes to that device. |

Sleep and USB: "USB host connected" means a computer set up the keyboard over USB, also
while it sleeps. Known limit: pulled from a sleeping computer, the keyboard can stay awake
until a key is pressed with the output on USB, or it is plugged in again. One difference: while
the keyboard types over Bluetooth to another device and the computer has had USB asleep
for about a minute, the computer does not keep the keyboard awake (a key press goes to the
other device and could not wake the computer anyway), so it sleeps then, if the keyboard
noticed that the computer put USB to sleep (some Linux sleeps do not show it). The minute
is for a Bluetooth link to the same computer: when that computer sleeps, the link drops
within at most 32 s and the output goes back to USB, so the keyboard stays awake and a key
press can wake the computer. A computer that is awake keeps it awake whatever the output,
unless it suspends the keyboard's USB port to save power (USB autosuspend). Test images
([CONTRIBUTING.md](../CONTRIBUTING.md#testing--verification)) never sleep; they keep both
settings without effect.

## For developers

The page is `web/config/index.html`, one file without a build step, in sections: SMP +
CBOR, USB transport, Bluetooth transport, config model, simulated keyboard, labels and help,
UI. Labels and help live in the page (the firmware sends keys, types and ranges only), so a
new setting needs a row in the labels section, or the page hides it (the key must also have
the type the page expects). The value checks follow `rainy75_cfg.py`, and the simulated
keyboard (`?demo`) follows `zmk/src/config/cfg_mgmt.c`. `?demo=future` adds a setting and an
effect this page does not know, `?demo=old` simulates a firmware without the settings and
`?demo=test` an unconfirmed (test) image.

The file stays under 100000 bytes, opens with a Content Security Policy meta tag that blocks
every network request, and uses no network API; the static tests check all three.

Tests: `node web/config/test-node.mjs` (Node 18 or later) runs everything but the UI; for
the UI, serve `web/` (`cd web && python3 -m http.server 8765`) and open
`http://localhost:8765/config/test.html` in Chrome (the title shows PASS or FAIL). The
workflow `.github/workflows/pages.yml` runs the Node tests, publishes `web/` on pushes to
`main` and attaches the page to every release. The protocol the page speaks is in
[config-protocol.md](config-protocol.md).
