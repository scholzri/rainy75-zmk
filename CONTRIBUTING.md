# Contributing

Thanks for your interest! Keymap tweaks, new RGB effects, sibling-board ports, driver
fixes, layout variants, and documentation are all welcome.

## Especially wanted

The plan to 1.0 and the points where help is welcome are in the pinned
**[Road to 1.0](https://github.com/scholzri/rainy75-zmk/issues/42)** issue. Most wanted
right now:

- **Test reports for the open Bluetooth controller** on Windows, macOS, iOS and other
  phones (template in the roadmap issue). "Works for me" counts too.
- **Other regional ISO layouts** (UK, Nordic, …) — mostly a keymap change.
- **Sibling boards** on the same Evision/Telink platform (CIDOO, IQUNIX, Ajazz,
  EPOMAKER — see [docs/evision-platform.md](docs/evision-platform.md)).

## Development setup

The full toolchain is described in [docs/zmk-firmware.md](docs/zmk-firmware.md). Summary:

- **Arch Linux distrobox** for the build (`pacman`, not `dnf`).
- **Zephyr SDK 0.17.0** (exactly) at `toolchain/zephyr-sdk-0.17.0`.
- A west workspace using `zmk/west.yml` as the manifest (ZMK + `hal_telink` + MCUboot are
  fetched at **pinned** revisions for reproducibility), plus a `.venv`.
- The default build uses the open BLE controller and no blob. The proprietary **Telink
  BLE blob** is not in the repo; only `./build.sh --blob` fetches it (`fetch_ble_blob.sh`,
  pinned + SHA-256 verified). Don't commit it: `.gitignore` covers `zmk/lib/*.a` (see
  [NOTICE](NOTICE)).

Build and test:

```bash
./build.sh -a --iso                             # MCUboot + app + combined + OTA + bridge (--ansi for ANSI)
./build.sh -p --iso                             # pristine app rebuild
./zmk/src/rainy_rgb/tests/run_host_tests.sh     # host unit tests: lighting engine (color/effects/overlay/lighting/ble_status)
./zmk/src/config/tests/run_host_tests.sh        # host unit tests: runtime settings (registry, codec, table)
./zmk/src/os_key/tests/run_host_tests.sh        # host unit tests: OS key behavior (Win/Mac swap, GUI lock)
./zmk/src/ble_open_profile/tests/run_host_tests.sh   # host unit tests: open profile timeout
./zmk/drivers/bluetooth/openll/tests/run_host_tests.sh   # host unit tests: open BLE link layer
(cd reverse/tools && python3 -m unittest test_rainy75_cfg test_rainy75_dfu test_rainy75_rgb test_openll_stats test_ble_adv_report)
node web/config/test-node.mjs                   # config page, all but the UI (Node 18 or later)
```

The config page's UI tests run in a browser: `cd web && python3 -m http.server 8765`, then
open `http://localhost:8765/config/test.html` in Chrome (the title shows PASS or FAIL).

## How the code is organized

All firmware code we add lives **out-of-tree** under [`zmk/`](zmk/) so the pinned ZMK/Zephyr
can be bumped without losing our work; the config page is the one exception, a separate web
page under `web/config/` that no firmware build touches:

```
zmk/boards/rainy75/     # board: DTS, keymap, defconfig, physical layout
zmk/drivers/            # BLE / USB / LED-strip / battery / watchdog
zmk/src/rainy_rgb/      # custom RGB lighting engine (host-tested)
zmk/src/config/         # runtime settings: registry, codec, table, storage, mcumgr group 67 (host-tested)
zmk/src/os_key/         # OS key decisions: Win/Mac swap, GUI lock (host-tested)
web/config/             # config page: one HTML file, no build step (Node and browser tests)
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

- **Host tests must pass:** the `run_host_tests.sh` of the part you touch (lighting engine,
  runtime settings, OS key, open BLE controller), the Python tests for anything under
  `reverse/tools/`, and `node web/config/test-node.mjs` plus `web/config/test.html` for the
  config page.
- **Builds must be clean:** `./build.sh -a --iso` (and `--ansi` if you touched the layout).
- **Hardware-affecting changes** (drivers, pins, timing, power, BLE) should be verified on
  a real board, and the PR should say how (what you observed: USB enumerates, BLE pairs,
  LEDs render correctly, no regressions, etc.). If you can't test on hardware, say so —
  it can still be merged as clearly-marked, untested support.
- **Flash untested images as test images.** Build with `--test-image`, then:

  ```bash
  python3 reverse/tools/rainy75_dfu.py upload build/zephyr/zmk.signed.bin --test --reset
  ```

  (or with the mcumgr CLI: `image upload`, `image list` for the slot 1 hash,
  `image test <hash>`, `reset`).

  The image never confirms itself, so if it breaks USB, Bluetooth or both, any reset or
  power cycle brings back the previous image. Test images also never sleep: waking from
  deep sleep is a cold boot and would bring the previous image back too (with USB unplugged, the wireless switch under
  the CapsLock keycap cuts the power). When everything works, make it permanent with
  `python3 reverse/tools/rainy75_dfu.py confirm` (or `mcumgr ... image confirm <hash>`). Don't upload another image while a test image runs
  unconfirmed: slot 1 holds your fallback. Reset back to it first. With the open Bluetooth
  controller, updates also work over BLE, a second way back if USB stops working.

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
`#ifdef` can't drive it, because devicetree is preprocessed before Kconfig runs. For the
C side (the per-key RGB LED map), `--ansi` additionally sets
`CONFIG_RAINY_RGB_ANSI_LEDMAP=y`, which selects the ANSI-calibrated tables in
`zmk/src/rainy_rgb/led_map.c`.

The keymap + matrix positions were **verified on real ANSI hardware** by
[@jaxx2104](https://github.com/jaxx2104) ([#1](https://github.com/scholzri/rainy75-zmk/issues/1)):
the wide Enter is `RC(3,13)`, the key above it is Backslash (`RC(2,13)`), and the ISO `<>`
slot (`RC(4,1)`) is unpopulated. (Originally derived from the vendor VIA layout,
[trkw/rainy75-v2-json](https://github.com/trkw/rainy75-v2-json).)

The per-key RGB `led_map` was **calibrated and verified on real ANSI hardware** by
[@ecliptik](https://github.com/ecliptik)
([#4](https://github.com/scholzri/rainy75-zmk/issues/4),
[#5](https://github.com/scholzri/rainy75-zmk/pull/5)): the ANSI WS2812 chain has **81
LEDs** (no LED under the ISO `<>` slot or the key right of Space), so ANSI builds use
their own `pos_to_led[]`/XY tables. The Enter-cluster chain detour is identical on both
PCBs. If you re-calibrate (e.g. for a sibling board), a temporary walker effect — one LED
lit, any keypress steps the chain — is the quickest ground truth.

## Licensing

By contributing you agree your contributions are licensed under **Apache-2.0**
([LICENSE](LICENSE)). Don't add code or assets you don't have the right to release under
it, and don't add proprietary vendor material (firmware, datasheets, decompiled code) —
see [NOTICE](NOTICE).
