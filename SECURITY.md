# Security Policy

## Official distribution

The **only** official home of this project is
**[github.com/scholzri/rainy75-zmk](https://github.com/scholzri/rainy75-zmk)**.

This project distributes **firmware source code only**. There is no official
binary download, `.exe`, `.zip`, or one-click installer. Installation is from
source via the documented methods in
[docs/zmk-firmware.md](docs/zmk-firmware.md) (OTA bridge → mcumgr DFU, or SWS
with a hardware debugger).

The one official web page is the settings page at
**[scholzri.github.io/rainy75-zmk/config/](https://scholzri.github.io/rainy75-zmk/config/)**,
also attached to each release as `rainy75-config.html` ([docs/config.md](docs/config.md)).
It changes settings on a connected keyboard and never offers a download.

## Impersonation / fake "firmware download" sites

Scam accounts copy this repository and publish GitHub Pages sites with a fake
"Download Latest Release" button that serves **malware** (typically a Windows
Lua-loader: `Application.bat` → `luau.exe` + an obfuscated Lua payload).

How to recognize a fake:

- It offers a `.zip`/`.exe` **download** — this project has none.
- It describes a "hold Escape, drag a `.uf2` onto the `RAINY75` USB drive" flow —
  the Wobkey Rainy 75 has **no UF2 / mass-storage bootloader**; that procedure
  is physically impossible on this hardware.
- It is not hosted under `github.com/scholzri` (the settings page: under
  `scholzri.github.io/rainy75-zmk/config/`).

### Known instances

| Date | Site | Payload SHA-256 | Status |
|------|------|-----------------|--------|
| 2026-07 | `hxxps://adventsundaysliminess908[.]github[.]io` (copied repo, no attribution) | `5d93a476398c736984b0e4de4091f65d0cba13f2d9153b04b2eaf16565df734c` | Reported to GitHub (abuse/DSA), VirusTotal (URL flagged malicious), Google Safe Browsing, Microsoft SmartScreen |

URLs above are defanged on purpose — do not visit them.

## Reporting

Found another impersonator, or a vulnerability in the firmware itself?
Please open an issue in this repository.
