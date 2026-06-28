# Installing the ZMK firmware

How to flash this ZMK firmware onto a Wobkey Rainy 75 Pro, how to go back to stock,
and how to build it yourself.

> ### ⚠️ Read this first
> - Flashing can **brick** the keyboard. There is **no warranty** ([LICENSE](LICENSE)).
> - **Get the stock firmware image before you start** (section 1) so you can go back —
>   no special hardware needed. This repo can't ship it (it's proprietary).
> - If a flash goes wrong and USB stops enumerating, recovery needs a hardware
>   Telink burning board over the SWS pads — see [docs/recovery.md](docs/recovery.md).
> - These instructions are for **Linux**. macOS/Windows aren't covered (the helper
>   scripts are bash + `mcumgr`).

---

## 0. What you need

- A Rainy 75 Pro on **stock firmware** (USB id `320f:5055`).
- A USB-C cable and a Linux machine.
- [`mcumgr`](https://github.com/apache/mynewt-mcumgr-cli) on your `PATH`
  (`go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest`).
- USB permissions: install the included udev rules so you don't need `sudo`:
  ```bash
  sudo cp 99-rainy75-zmk.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```
- The firmware images **`build/combined.bin`** (MCUboot + ZMK app) and
  **`build-bridge/bridge_ota.bin`** (the OTA transition image), produced by building from
  source — see [section 4](#4-build-from-source).

> **Why no prebuilt download?** The firmware links the proprietary Telink BLE blob, so a
> prebuilt binary would embed — and thus redistribute — it, which its license forbids
> (see [NOTICE](NOTICE)). Everyone builds from source; the build fetches the blob from
> Telink for you.

---

## 1. Get the stock firmware image (so you can go back)

You don't need to dump your own keyboard to be able to restore it — and you can't over
USB anyway (the stock OTA protocol is **write-only**). What `restore_stock.sh` needs is
the **stock firmware image**, which is the same for every unit of this model. Your
per-device calibration + Bluetooth MAC live separately (flash `0xFE000`) and are never
touched by a restore. Get the image one of two ways:

**Option A — extract it from the official updater (no special hardware).** The vendor
ships firmware as a Windows updater `.exe` — a .NET app with the firmware embedded as a
resource named `code_2M`. You don't *run* it; you just pull the image out (works on Linux):

1. **Download** the official **ISO** updater `.exe` — see the link in
   [docs/resources.md](docs/resources.md) (we don't host it). Make sure it matches your
   layout (ISO vs ANSI) and is the ISO build.
2. **Extract** the OTA image (one-time `pip install dnfile`):
   ```bash
   python3 reverse/tools/extract_stock_firmware.py "Rainy 75 ISO firmware.exe" \
       -o reverse/firmware/firmware_ota.bin
   ```
   The script finds the `code_2M` resource, locates the Telink boot header (`TLNK` magic),
   reads the image size from it, and writes the ~120 KB OTA payload — verified
   byte-identical to a real flash dump. It prints the SHA-256 so you can confirm.
   *Manual alternative:* open the `.exe` in **ILSpy** or **dnSpy**, save the `code_2M`
   resource to a file, and keep everything from the Telink boot vector onward (the payload
   starts at offset `0x104` inside the resource, right before the `TLNK` magic at `0x124`).

**Option B — dump your own flash over SWS** with a Telink burning board (a full
per-device backup, including calibration) — see [docs/recovery.md](docs/recovery.md).

> This repository does **not** ship the stock firmware — it is proprietary
> (Telink / Wobkey / Evision) and cannot be redistributed. Download it from the official
> source.

Keep that file safe. `restore_stock.sh` needs it.

---

## 2. Install ZMK  (`./install_zmk.sh`)

With the keyboard plugged in on stock firmware:

```bash
./build.sh -a --iso    # build the images (section 4); use --ansi for the ANSI layout
./install_zmk.sh       # interactive — explains each step and asks before writing
```

What it does (all over USB, **no debugger**):

1. OTA-flashes a small **bridge** firmware via the stock update protocol (runs from
   flash bank 1).
2. The bridge writes the full **MCUboot + ZMK** image to `0x0` via a RAM trampoline,
   then resets into ZMK.

Takes ~1–2 minutes. When it's done the keyboard re-enumerates as `1d50:615e`
("Rainy 75 Pro") and types immediately.

Non-interactive / custom images:
```bash
./install_zmk.sh -y
./install_zmk.sh --bridge build-bridge/bridge_ota.bin --zmk build/combined.bin
```

If `/dev/ttyACM0` is taken or the port differs, set `SERIAL_PORT=/dev/ttyACM1 ./install_zmk.sh`.

---

## 3. Go back to stock  (`./restore_stock.sh`)

Needs the stock image from [section 1](#1-back-up-your-stock-firmware-do-not-skip) at
`reverse/firmware/firmware_ota.bin`:

```bash
./restore_stock.sh                       # interactive
./restore_stock.sh -y                     # non-interactive
./restore_stock.sh path/to/firmware.bin   # custom image
```

It writes the stock firmware to flash `0x0` via the ZMK `flash_mgmt` mcumgr group and
resets. Your calibration + MAC at `0xFE000` are left untouched.

---

## 4. Build from source

The full build environment (west workspace, Zephyr SDK, Telink HAL, patches) is
documented in **[docs/zmk-firmware.md](docs/zmk-firmware.md)**. In short:

- **Zephyr SDK 0.17.0** (exactly — not 0.17.4) at `toolchain/zephyr-sdk-0.17.0`.
- A west workspace with this repo as the manifest (`zmk/west.yml` fetches ZMK,
  `hal_telink`, and MCUboot at pinned revisions), plus a Python venv (`.venv`).
- The project is built inside an **Arch Linux distrobox** (`pacman`, not `dnf`).
- The **Telink BLE blob** is **not in this repo** (proprietary — see [NOTICE](NOTICE));
  `build.sh` auto-fetches it (`fetch_ble_blob.sh`, pinned + SHA-256 verified) on the first
  build, so the initial build needs network access.
- **One-time west setup** after `west update` — install the Python deps and export the
  Zephyr CMake package, or the build won't configure:
  ```bash
  pip install -r zephyr/scripts/requirements.txt
  west zephyr-export
  ```

Then:

```bash
./build.sh -a --iso     # MCUboot + ZMK app + combined.bin + OTA + bridge (--ansi for ANSI)
./build.sh -p --iso     # pristine app rebuild
```

Outputs: `build/combined.bin`, `build-bridge/bridge_ota.bin`, and
`build/zephyr/zmk.signed.bin` (for plain mcumgr DFU updates once you're already on ZMK).

Updating an already-ZMK keyboard (no bridge needed):
```bash
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" \
    image upload build/zephyr/zmk.signed.bin
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" image test <hash>
mcumgr --conntype serial --connstring "dev=/dev/ttyACM0,baud=115200" reset
```

### ANSI layout

Pass `--iso` or `--ansi` to any app build — there is **no default**, so an ANSI board
can't silently get an ISO build (or vice-versa):

```bash
./build.sh -a --ansi      # ANSI
./build.sh -a --iso       # ISO DE
```

ANSI puts the wide **Enter** on its real matrix cell `RC(3,13)`, makes the key above it
**Backslash**, and drops the ISO `<>` key. These matrix positions were **verified on real
ANSI hardware** by [@jaxx2104](https://github.com/jaxx2104) (issue #1). Still being
confirmed: the per-key **RGB** mapping around the Enter cluster — if you run ANSI, reports
welcome ([CONTRIBUTING.md](CONTRIBUTING.md#layout-variants-iso--ansi)).

---

## 5. Customizing

- **Keymap:** `zmk/boards/rainy75/rainy75.keymap` (standard ZMK devicetree keymap).
- **RGB:** controls and the lighting engine are in [docs/rainy-rgb.md](docs/rainy-rgb.md).
- Rebuild (`./build.sh -p`) and re-flash via mcumgr (section 4).

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `/dev/ttyACM0` missing | Re-plug; check `dmesg`; some steps re-enumerate the device — wait a few seconds. |
| Permission denied on the port | Install the udev rules (section 0) or run with `sudo`. |
| `mcumgr: command not found` | Install it and ensure `~/go/bin` is on `PATH`. |
| Install stalls mid-step | Re-plug and re-run — the two stages are each idempotent. |
| Keyboard won't enumerate at all after a bad flash | Hardware recovery: [docs/recovery.md](docs/recovery.md). |
