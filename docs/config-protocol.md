# Runtime Settings Protocol (mcumgr group 67)

The keyboard's runtime settings are read and written over mcumgr (SMP) group 67,
on the same transports as firmware updates: the USB console port (interface 0) and
Bluetooth (SMP GATT service `8d53dc1d-1db7-4cd3-868b-8a527460aa84`, characteristic
`da2e7828-fbce-4e01-ae9e-261174997c48`). Clients: `reverse/tools/rainy75_cfg.py` and the
config page.

Firmware: `zmk/src/config/` (registry `cfg_registry.c`, table `cfg_table.c`, storage
`cfg_store.c` with its stored form in `cfg_codec.c`, this protocol `cfg_mgmt.c`), enabled
with `CONFIG_RAINY75_CONFIG`.

## Commands

| # | Op | Request | Response |
|---|---|---|---|
| 0 | read `info` | `{}` | `{rc, v, n, fx, rev}` |
| 1 | read `list` | `{i?}` | `{rc, s: [[key, type, a, b, flags], ...], next?}` |
| 2 | read `get` | `{i?}` or `{k: [key, ...], i?}` | `{rc, v: {key: value, ...}, next?}` |
| 3 | write `set` | `{k: key, v: value}` | `{rc, v: value as stored}` |
| 4 | write `reset` | `{}` (all writable) or `{k: [key, ...]}` | `{rc}` |

- Encoding: every request carries a CBOR map, `{}` when there are no fields (`list`, `get`,
  `set` and `reset` refuse an empty payload with `EINVAL`). Replies may use definite- or
  indefinite-length arrays and maps (today's build uses indefinite); a client's CBOR decoder
  must accept both.
- Access: over USB the group is reachable without authentication, the same as firmware
  upload over USB. Over Bluetooth it needs an authenticated bond
  (`CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN`, Zephyr's default with `BT_SMP`).
- `info`: `v` protocol version (1), `n` number of settings, `fx` the effect names in
  table order (as built), `rev` a change counter that changes on every change from any
  source (host, Fn keys). Hosts poll `info` and re-read values when `rev` changes. `rev`
  is not persistent: it restarts at 0 on every boot (loading `rgb.boot_effect` can already
  count as one change), so clients compare it for inequality only and re-read everything
  after any (re)connect.
- `list` / `get` page: a reply holds as many entries as fit one mcumgr buffer (512 bytes),
  at least one; `next` is the index to ask for next, absent on the last page. `i` past the
  end gives an empty page (`s: []` or `v: {}`) and no `next`. The page size is an internal
  estimate (a page may hold fewer entries than would fit), so clients follow `next` and
  never assume a page size.
- `get` with `k` pages too: `i` is an index into the requested key list and `next` is the
  index into that list to continue from. A client continues either mode by re-sending the
  same request with `i = next`. A key listed twice is answered once. An unknown key anywhere
  in `k` fails the whole request with `ENOENT` (no partial reply); more than 32 keys in `k`
  give `EINVAL`.
- `k: []` (present but empty) reads or resets nothing; an absent `k` means all (for `reset`:
  all writable settings).
- `set` applies immediately; storage is written 2 s after the last change. A `set` or
  `reset` of any setting that lives in the rainy_rgb state record (`rgb.on`, `rgb.effect`,
  `rgb.hue`, `rgb.sat`, `rgb.val`, `rgb.speed`) leaves host pixel mode (group 65 direct
  mode), so the new value is visible; the other settings do not. Those six and the
  lighting and indicator settings (`rgb.val_battery`, `rgb.idle_s`, `rgb.idle_mode`,
  `ind.*`) also restart the lighting idle timer (`rgb.idle_s`), so a change shows on an idle
  board. `rgb.boot_effect` takes effect at the next boot, `rgb.cycle` at the next Fn+Enter.
- `reset` with keys checks all of them first (unknown: `ENOENT`, read-only:
  `EACCESSDENIED`) and resets none if one fails. Should resetting a checked key still fail
  (defaults are validated at boot, so this is not expected), the remaining keys are reset
  and saved anyway and the reply carries the first error.

## Types

| `type` | Value | `a` | `b` |
|---|---|---|---|
| `b` | CBOR bool | null | null |
| `u` | CBOR unsigned | min | max |
| `e` | CBOR text, one of `a` | list of names | null |
| `c` | CBOR unsigned `0xRRGGBB` | null | null |
| `l` | CBOR list of texts, a subset of `a` in order | list of names | null |

`l`: at most 16 entries; a longer request is refused with `EINVAL`, even if it contains
duplicates. The order is the client's order (kept as sent); duplicates are dropped within
those 16, the first one kept, and `set` echoes the result.

`flags`: bit 0 = read-only; the other bits are reserved, clients ignore them. Names are
identifiers, not display text; clients own labels.

## Errors

A failed request's reply is a map with only `rc` (non-zero); clients check `rc` before
reading any other field.

mcumgr `rc`: 3 `EINVAL` (wrong type, out of range, unknown name, list too long, bad
request), 5 `ENOENT` (unknown key), 7 `EMSGSIZE` (reply too large), 8 `ENOTSUP` (no such
group, command or op, see below), 11 `EACCESSDENIED` (read-only; `set` returns it before the
value is checked). A valid value that needs normalization is normalized and echoed (for
example a list without its duplicates).

`ENOTSUP` is how a client detects old firmware: a build without group 67 (the fallback
image, v0.3.x) answers every request of this group with rc 8. The same rc comes from a
command number this build does not have or from the wrong op (for example `set` sent as a
read).

## Settings

| Key | Type | Range / names | Default | Storage |
|---|---|---|---|---|
| `rgb.on` | b | | on | rainy_rgb state record |
| `rgb.effect` | e | the effects of `fx` | `solid` | rainy_rgb state record |
| `rgb.hue` | u | 0..255 | 0 | rainy_rgb state record |
| `rgb.sat` | u | 0..255 | 255 | rainy_rgb state record |
| `rgb.val` | u | 16..255 | 200 | rainy_rgb state record |
| `rgb.speed` | u | 1..255 | 32 | rainy_rgb state record |
| `rgb.boot_effect` | e | `last` + the effects | `last` | `rainy_cfg/rgb.boot_effect` |
| `rgb.cycle` | l | the effects of `fx` | all effects in table order | `rainy_cfg/rgb.cycle` |
| `rgb.val_battery` | u | 16..255 | 255 (no cap) | `rainy_cfg/rgb.val_battery` |
| `rgb.idle_s` | u | 0..3600 (0 = never) | 0 | `rainy_cfg/rgb.idle_s` |
| `rgb.idle_mode` | e | `off`, `dim` | `off` | `rainy_cfg/rgb.idle_mode` |
| `ind.caps_style` | e | `key`, `tint`, `off` | `key` | `rainy_cfg/ind.caps_style` |
| `ind.caps_color` | c | | `0xFFFFFF` | `rainy_cfg/ind.caps_color` |
| `ind.fn_highlight` | b | | on | `rainy_cfg/ind.fn_highlight` |
| `ind.passkey_guide` | b | | on | `rainy_cfg/ind.passkey_guide` |
| `ind.bat_low` | u | 0..50 (%, 0 = off) | 0 | `rainy_cfg/ind.bat_low` |

- `rgb.boot_effect`: the effect shown after power-on; `last` keeps the effect last chosen.
- `rgb.cycle`: the effects Fn+Enter steps through, in this order: the entry after the
  current effect (wrapping), or the first entry when the current effect is not listed. An
  empty list (`[]`) means all effects in table order. `set` refuses names this firmware does
  not have; when loading, they are skipped (a list of only unknown names loads as empty).
- `rgb.val_battery`: while no USB host is connected the effect renders at
  `min(rgb.val, rgb.val_battery)`; `rgb.val` itself does not change. "USB host connected"
  means a host has configured the keyboard, also while the host sleeps (ZMK
  `zmk_usb_is_hid_ready()`). Known limit: if the host put the USB bus to sleep and the
  cable is then pulled, the keyboard can still count as connected (no cap, no low-battery
  pulse) until the next USB event; with output USB the first keypress clears it, with
  output Bluetooth it can persist.
- `rgb.idle_s` / `rgb.idle_mode`: after `rgb.idle_s` seconds without a key event, the
  effect turns off (`off`) or renders at a quarter of its brightness (`dim`); the next key
  brings it back. The indicators (CapsLock, Fn highlight, battery gauge, Bluetooth status,
  passkey guide) keep showing, the CapsLock `tint` only with `dim`; host pixel mode is not
  affected.
- `ind.caps_style` / `ind.caps_color`: with CapsLock on, `key` lights the CapsLock key in
  the colour at full strength, `tint` mixes every LED 50/50 with the colour scaled to the
  brightness the effect is drawn at (after `rgb.val_battery`, idle `dim` and the fade during
  a Bluetooth animation), `off` shows nothing. The tint does not show while the Fn highlight
  does (Fn held, `ind.fn_highlight` on) or while the effect is not drawn (RGB off, idle
  `off`, a Bluetooth animation, host pixel mode).
- `ind.fn_highlight`: while the Fn layer is held, keys whose Fn-layer binding is not
  transparent light white and the rest go dark; off leaves the lighting as it is (F1..F4
  still show the Bluetooth slots). The keys come from the live keymap, so they follow ZMK
  Studio changes.
- `ind.passkey_guide`: off hides the number-row passkey guidance (digits, Enter pulse,
  verify chase, red digit flash); the F1..F4 slot status stays. With it off, nothing on the
  keyboard reacts to a passkey request until Enter is pressed, so the host's pairing dialog
  is the only cue that digits are expected. After Enter the slot being verified still
  blinks and the effect stays off until the pairing ends.
- `ind.bat_low`: while no USB host is connected (as for `rgb.val_battery`, with the same
  known limit) and the battery level is below this percentage, Esc pulses red (2 s period)
  on top of the effect; 0 = off. It shows only while the effect is drawn (RGB on, not idle
  `off`, not host pixel mode, not during a Bluetooth animation), and a battery level of 0
  (no reading yet) never pulses.

## Storage

Settings that are not part of the rainy_rgb state record are stored one entry per key under
the settings subtree `rainy_cfg`: bool, uint and colour as 4-byte little-endian, enum as its
name, list as names joined with `,`, and an empty list as a single `,` (an entry of length 0
would be a deletion and load as the default). A stored name the firmware does not know is
ignored (enum: default kept; list: name skipped). A setting at its default has no entry:
saving it deletes the entry, so `reset` deletes, and a later firmware with a different
default applies its new default. A failed save is retried up to 3 times, 10 s apart, then
logged.

## Compatibility rules

1. A key is never renamed and never reused for something else.
2. A key's type never changes; a new type means a new key.
3. Ranges may only widen; names may only be added.
4. New effects are only appended to the effect table (stored effect indexes stay valid).
5. Clients hide keys and names they do not know; the firmware rejects keys it does not
   know.
6. Every new setting is added to the table above in the same PR.
