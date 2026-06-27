# Resources & References

## Official Downloads

| File | URL |
|------|-----|
| ISO Firmware (.exe) | `https://drivers.sfo3.digitaloceanspaces.com/Rainy75%20ISO%20firmware.exe` |
| VIA JSON (all models) | `https://driveall.oss-cn-hangzhou.aliyuncs.com/Rain75/JSON.zip` |
| VIA JSON ISO USB | `https://drivers.sfo3.digitaloceanspaces.com/Rainy%C2%A075-ISO-USB.zip` |
| VIA JSON ISO 2.4G | `https://drivers.sfo3.digitaloceanspaces.com/Rainy%C2%A075-ISO-2.4G.zip` |
| User Manual (EN) | `https://drivers.sfo3.digitaloceanspaces.com/Rainy_75_EN.pdf` |
| Support Page | `https://www.wobkey.com/pages/rainy_75_support` |
| Product Wiki | `https://wiki.wobkey.com/en/Products/Rainy-75/Overview` |
| Official Website | `https://www.wobkey.com/` |
| Driver/Firmware Portal | `https://www.woblab.cn` |
| Local Manual Copy | `reverse/reference/rainy75_manual.pdf` |

## Telink / SDK

- Telink B91 BLE SDK: https://github.com/telink-semi/tl_ble_sdk
- Telink Zephyr support: https://github.com/telink-semi/zephyr
- Telink B91 in Zephyr docs: https://docs.zephyrproject.org/latest/boards/telink/tlsr9518adk80d/doc/index.html
- Telink HID Solutions: https://wiki.telink-semi.cn/wiki/solution/HID-Solution/
- Telink BDT for Linux: https://wiki.telink-semi.cn/wiki/IDE-and-Tools/BDT_for_TLSR9_Series_in_Linux/

## Reference Projects

- **Telink RE precedent:** pvvx's ATC_MiThermometer (TLSR82xx, full rewrite, 4k stars)
- **Closest RE methodology:** Aleph Security Kontrol Lux smart lock (TLSR8251, Ghidra RE)
- **BSim for LTO matching:** Pen Test Partners ESP32 case study
- **LTO binary diffing research:** ACM TOSEM 2022, "1-to-1 or 1-to-n?"

## Andes RISC-V

- Ghidra 12.0.1 built-in `andestar_v5.instr.sinc` — no custom extension needed
- AndeStar V5 ISA Spec UM165 v1.5.08 — `github.com/andestech/andes-v5-isa/releases/tag/ast-v5_4_0-release`
- LLVM Andes encodings: `RISCVInstrInfoXAndes.td`

## Evision Platform / Sibling Keyboards

- GearHub (Attack Shark config tool): https://qmk.top
- WOB Driver (Wobkey config tool): https://wobwxe.com
- AKS068 VIA JSON (xero): https://github.com/xero/aks068-via
- AKS068 VIA JSON (vzhny): https://github.com/vzhny/aks068-via
- VIA JSON collection (since19861019): https://github.com/since19861019/via-json
- EPOMAKER EK21 VIA bug report: https://github.com/the-via/keyboards/issues/2237
- CIDOO ABM066 VIA JSON: https://epomaker.com/blogs/via-json/cidoo-abm066-usb-via-json-file

## Keyboard Community

- ZMK Firmware: https://zmk.dev/docs
- ZMK Hardware Integration: https://zmk.dev/docs/development/hardware-integration
- QMK GPL enforcement issue: https://github.com/qmk/qmk_firmware/issues/24085
- Evision USB VID info: https://the-sz.com/products/usbid/index.php?v=0x320F
- SonixQMK Mechanical Keyboard Database: https://github.com/SonixQMK/Mechanical-Keyboard-Database
