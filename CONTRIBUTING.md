# Contributing

Thanks for your interest! Keymap tweaks, new RGB effects, sibling-board ports, driver
fixes, layout variants, and documentation are all welcome.

## Especially wanted

- **ANSI layout verification.** This board ships in both ISO and ANSI variants. The
  firmware was developed and hardware-tested on **ISO DE**. ANSI support is being added
  but needs someone with an **ANSI** board to confirm the keymap and per-key RGB mapping
  on real hardware — see [the layout note below](#layout-variants-iso--ansi).
- **Other regional ISO layouts** (UK, Nordic, …) — mostly a keymap change.
- **Sibling boards** on the same Evision/Telink platform (CIDOO, IQUNIX, Ajazz,
  EPOMAKER — see [docs/evision-platform.md](docs/evision-platform.md)).

## Development setup

The full toolchain is described in [docs/zmk-firmware.md](docs/zmk-firmware.md). Summary:

- **Arch Linux distrobox** for the build (`pacman`, not `dnf`).
- **Zephyr SDK 0.17.0** (exactly) at `toolchain/zephyr-sdk-0.17.0`.
- A west workspace using `zmk/west.yml` as the manifest (ZMK + `hal_telink` + MCUboot are
  fetched at **pinned** revisions for reproducibility), plus a `.venv`.
- The proprietary **Telink BLE blob** is not in the repo; `build.sh` fetches it
  (`fetch_ble_blob.sh`, pinned + SHA-256 verified). Don't commit it — `.gitignore` covers
  `zmk/lib/*.a` (see [NOTICE](NOTICE)).

Build and test:

```bash
./build.sh -a                                   # MCUboot + app + combined + OTA + bridge
./build.sh -p                                   # pristine app rebuild
./zmk/src/rainy_rgb/tests/run_host_tests.sh     # host unit tests (color/effects/overlay)
```

## How the code is organized

Everything we add lives **out-of-tree** under [`zmk/`](zmk/) so the pinned ZMK/Zephyr can
be bumped without losing our work:

```
zmk/boards/rainy75/     # board: DTS, keymap, defconfig, physical layout
zmk/drivers/            # BLE / USB / LED-strip / battery / watchdog
zmk/src/rainy_rgb/      # custom RGB lighting engine (host-tested)
conf/                   # app / mcuboot / ota-bridge config overlays
patches/                # minimal upstream patches, applied by build.sh
```

Keep this boundary: prefer a new file under `zmk/` over editing fetched sources in
`zmk-src/` or `zephyr/`. If you genuinely must touch upstream, add a patch under
`patches/` instead of an in-place edit.

## Making changes

- **Keymap:** `zmk/boards/rainy75/rainy75.keymap`.
- **RGB effects:** add to `zmk/src/rainy_rgb/effects.{c,h}` and the registry; the effects
  are **pure functions** with host tests — add a test in `zmk/src/rainy_rgb/tests/`.
  Architecture: [docs/rainy-rgb.md](docs/rainy-rgb.md).
- **Drivers / pins / timing:** these are hardware-specific — say how you verified the
  change (see below).

## Testing & verification

- **Host tests must pass:** `run_host_tests.sh` for anything touching the engine.
- **Builds must be clean:** `./build.sh -a`.
- **Hardware-affecting changes** (drivers, pins, timing, power, BLE) should be verified on
  a real board, and the PR should say how (what you observed: USB enumerates, BLE pairs,
  LEDs render correctly, no regressions, etc.). If you can't test on hardware, say so —
  it can still be merged as clearly-marked, untested support.

## Pull requests

1. Branch off `main`.
2. Keep commits focused; use clear messages (we use Conventional-Commits-style prefixes:
   `feat(rainy_rgb): …`, `fix(led_strip): …`, `docs: …`).
3. In the PR description, note **how you verified** it (host tests, build, hardware).
4. Don't commit build artifacts, fetched modules, the toolchain, or any
   proprietary/vendor material — `.gitignore` covers these; please keep it that way.

## Layout variants (ISO / ANSI)

An **experimental ANSI variant** already exists: build with `-DCONFIG_RAINY75_ANSI=y`
(see [INSTALL.md](INSTALL.md#ansi-layout-experimental-untested)). It drops the ISO `<>`
key (full-width Left-Shift) and remaps the Enter / backslash area in
`zmk/boards/rainy75/rainy75.keymap` (the `K_ENTER_TOP` / `K_HASH` / `K_LT_GT` macros).

It was derived from the vendor VIA layout
([trkw/rainy75-v2-json](https://github.com/trkw/rainy75-v2-json)) but is **not verified on
real ANSI hardware**. What needs confirming on an actual ANSI board:

- **The Enter / backslash matrix mapping.** The vendor VIA places the ANSI Enter on a
  matrix node our ISO transform skips (`RC(3,13)`), while our ISO Enter is `RC(2,13)`.
  The current variant assumes ANSI reuses the ISO switches (Enter→`RC(3,12)`,
  backslash→`RC(2,13)`); if the ANSI board instead populates `RC(3,13)`, the board's
  `matrix-transform` in `rainy75.dts` needs a small change too.
- **The per-key RGB `led_map`** near Enter (the spatial effects use XY positions).

If you have an **ANSI** Rainy 75, please test and open an issue or PR — that's the one
thing blocking first-class ANSI support.

## Licensing

By contributing you agree your contributions are licensed under **Apache-2.0**
([LICENSE](LICENSE)). Don't add code or assets you don't have the right to release under
it, and don't add proprietary vendor material (firmware, datasheets, decompiled code) —
see [NOTICE](NOTICE).
