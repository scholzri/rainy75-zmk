# Contributing

Thanks for your interest! Keymap tweaks, new RGB effects, sibling-board ports, driver
fixes, layout variants, and documentation are all welcome.

## Especially wanted

- **ANSI per-key RGB.** This board ships in both ISO and ANSI variants. ANSI is supported,
  and its keymap + matrix were verified on real hardware by a contributor; the one piece
  still unconfirmed is the per-key **RGB** mapping around the Enter cluster — see [the
  layout note below](#layout-variants-iso--ansi).
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
./build.sh -a --iso                             # MCUboot + app + combined + OTA + bridge (--ansi for ANSI)
./build.sh -p --iso                             # pristine app rebuild
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
- **Builds must be clean:** `./build.sh -a --iso` (and `--ansi` if you touched the layout).
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

Both layouts are supported; pick one at build time — there is **no default**:

```bash
./build.sh -a --iso     # ISO DE
./build.sh -a --ansi    # ANSI
```

ANSI drops the ISO `<>` key (full-width Left-Shift) and remaps the Enter / backslash area
via the `K_ENTER_TOP` / `K_HASH` / `K_LT_GT` macros in `rainy75.keymap`, plus a conditional
row in the matrix transform in `rainy75.dts`. Selection rides the devicetree-preprocessor
symbol `RAINY75_ANSI` (`--ansi` passes `-DDTS_EXTRA_CPPFLAGS=-DRAINY75_ANSI`) — a Kconfig
`#ifdef` can't drive it, because devicetree is preprocessed before Kconfig runs.

The keymap + matrix positions were **verified on real ANSI hardware** by
[@jaxx2104](https://github.com/jaxx2104) ([#1](https://github.com/scholzri/rainy75-zmk/issues/1)):
the wide Enter is `RC(3,13)`, the key above it is Backslash (`RC(2,13)`), and the ISO `<>`
slot (`RC(4,1)`) is unpopulated. (Originally derived from the vendor VIA layout,
[trkw/rainy75-v2-json](https://github.com/trkw/rainy75-v2-json).)

**Still open:** the per-key RGB `led_map` near Enter (the spatial effects use XY positions).
If you run ANSI, confirming the LEDs light the right physical keys around the Enter cluster
would close this out — please open an issue or PR.

## Licensing

By contributing you agree your contributions are licensed under **Apache-2.0**
([LICENSE](LICENSE)). Don't add code or assets you don't have the right to release under
it, and don't add proprietary vendor material (firmware, datasheets, decompiled code) —
see [NOTICE](NOTICE).
