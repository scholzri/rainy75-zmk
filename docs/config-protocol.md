# Runtime Settings Protocol (mcumgr group 67)

The keyboard's runtime settings are read and written over mcumgr (SMP) group 67,
on the same transports as firmware updates: the USB console port (interface 0) and
Bluetooth (SMP GATT service `8d53dc1d-1db7-4cd3-868b-8a527460aa84`, characteristic
`da2e7828-fbce-4e01-ae9e-261174997c48`). Clients: `reverse/tools/rainy75_cfg.py` and the
config page.

Firmware: `zmk/src/config/` (registry `cfg_registry.c`, table `cfg_table.c`, storage
`cfg_store.c`, this protocol `cfg_mgmt.c`), enabled with `CONFIG_RAINY75_CONFIG`.

## Commands

| # | Op | Request | Response |
|---|---|---|---|
| 0 | read `info` | `{}` | `{rc, v, n, fx, rev}` |
| 1 | read `list` | `{i?}` | `{rc, s: [[key, type, a, b, flags], ...], next?}` |
| 2 | read `get` | `{i?}` or `{k: [key, ...], i?}` | `{rc, v: {key: value, ...}, next?}` |
| 3 | write `set` | `{k: key, v: value}` | `{rc, v: value as stored}` |
| 4 | write `reset` | `{}` (all writable) or `{k: [key, ...]}` | `{rc}` |

- Encoding: every request carries a CBOR map, `{}` when there are no fields (`list`, `get`,
  `set` and `reset` refuse an empty payload with `EINVAL`). Replies use indefinite-length
  arrays and maps, so a client's CBOR decoder must accept them.
- `info`: `v` protocol version (1), `n` number of settings, `fx` the effect names in
  table order (as built), `rev` a change counter that changes on every change from any
  source (host, Fn keys). Hosts poll `info` and re-read values when `rev` changes.
- `list` / `get` page: a reply holds as many entries as fit one mcumgr buffer (512 bytes),
  at least one; `next` is the index to ask for next, absent on the last page. `i` past the
  end gives an empty page (`s: []` or `v: {}`) and no `next`.
- `get` with `k` pages too: `i` is an index into the requested key list and `next` is the
  index into that list to continue from. A client continues either mode by re-sending the
  same request with `i = next`. A key listed twice is answered once.
- `set` applies immediately; storage is written 2 s after the last change.
- `reset` with keys checks all of them first (unknown: `ENOENT`, read-only:
  `EACCESSDENIED`) and resets none if one fails.

## Types

| `type` | Value | `a` | `b` |
|---|---|---|---|
| `b` | CBOR bool | null | null |
| `u` | CBOR unsigned | min | max |
| `e` | CBOR text, one of `a` | list of names | null |
| `c` | CBOR unsigned `0xRRGGBB` | null | null |
| `l` | CBOR list of texts, a subset of `a` in order | list of names | null |

`flags`: bit 0 = read-only. Names are identifiers, not display text; clients own labels.

## Errors

mcumgr `rc`: 3 `EINVAL` (wrong type, out of range, unknown name, list too long, bad
request), 5 `ENOENT` (unknown key), 7 `EMSGSIZE` (reply too large), 11 `EACCESSDENIED`
(read-only; `set` returns it before the value is checked). A valid value that needs
normalization is normalized and echoed (duplicates in a list are dropped, the first one
kept).

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

`rgb.boot_effect`: the effect shown after power-on; `last` keeps the effect last chosen.

## Storage

Settings that are not part of the rainy_rgb state record are stored one entry per key under
the settings subtree `rainy_cfg`: bool, uint and colour as 4-byte little-endian, enum as its
name, list as names joined with `,`. A stored name the firmware does not know is ignored
(enum: default kept; list: name skipped).

## Compatibility rules

1. A key is never renamed and never reused for something else.
2. A key's type never changes; a new type means a new key.
3. Ranges may only widen; names may only be added.
4. New effects are only appended to the effect table (stored effect indexes stay valid).
5. Clients hide keys and names they do not know; the firmware rejects keys it does not
   know.
6. Every new setting is added to the table above in the same PR.
