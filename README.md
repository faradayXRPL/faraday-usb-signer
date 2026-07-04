# FARADAY USB Signer — Firmware v0.1

Offline XRPL transaction signer for **ESP32-S3 touch LCD** hardware
(480×320, USB-CDC).

Unsigned transactions arrive over **USB serial**, are shown field-by-field on the
display, signed locally after a physical *hold-to-sign*, and returned as a signed
`tx_blob` over the same USB line. **The seed never leaves the device.**

| Property | Detail |
|----------|--------|
| Network | XRPL **mainnet** only (other networks are rejected) |
| Radios | WiFi / BLE are never started |
| I/O | Display, touch, USB-CDC |
| Signing | Sign-what-you-see — fields are re-derived on-device; allowlisted types only |

Part of the [Faraday](https://github.com/faradayXRPL) air-gapped signing stack.
Use with the desktop companion
[faraday-electron-app](https://github.com/faradayXRPL/faraday-electron-app).

## Hardware

- **Board:** ESP32-S3 with integrated 3.5″ touch LCD (ST7796 display, FT6336 touch)
- **Flash:** 16 MB, OPI PSRAM
- **Connection:** USB data cable (CDC serial @ 115200 baud)

## Quick start

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/faradayXRPL/faraday-usb-signer.git
cd faraday-usb-signer
pio run -t upload          # build + flash over USB
pio device monitor         # optional: serial log
```

PlatformIO env: `esp32-s3-touch-lcd-3p5c` (`esp32-s3-devkitc-1` profile).

> This folder also lives in the monorepo
> [faraday-v0.1](https://github.com/faradayXRPL/faraday-v0.1) at
> `firmware/faraday-usb-signer`.

## USB protocol (115200 baud, line based)

Unsigned transactions use a length-prefixed frame so large payloads can't be
truncated by host buffering:

```
Host -> Device:  UNSIGNED <byteLength>\n
Host -> Device:  <json>\n                 (exactly <byteLength> bytes of JSON)
```

A small payload may also be sent inline on one line: `UNSIGNED {json}\n`.

| Direction | Message | Meaning |
|-----------|---------|---------|
| Host → Device | `PING` | Liveness check |
| Host → Device | `UNSIGNED <len>` + body | Submit unsigned tx for review |
| Device → Host | `READY usb-signer` | Sent once on boot |
| Device → Host | `PONG unlocked` / `PONG locked` | Reply to `PING` |
| Device → Host | `ACK` | Payload received, shown for review |
| Device → Host | `SIGNED {…}` (single line) | Signed payload (`tx_blob`, `tx_hash`) |
| Device → Host | `REJECTED` | User rejected on device |
| Device → Host | `ERROR <message>` | Parse/validation/sign failure |

Signed response (host verifies and broadcasts):

```json
{ "protocol": "XRPL-AQ/1", "kind": "signed", "network": "mainnet",
  "tx_blob": "…", "tx_hash": "…" }
```

Unsigned payload the device accepts:

```json
{ "network": "mainnet", "tx_json": { "TransactionType": "Payment", … } }
```

## Allowlisted transaction types

`Payment`, `OfferCreate`, `OfferCancel`, `TrustSet`, `AMMDeposit`, `AMMWithdraw`,
`AccountSet`. The companion console builds only these; the device enforces them.

## Source layout

```
src/       main, display, ui_*, theme, wallet, tx_signer, crypto_util, usb_link
include/   headers + config.h (pins, sizes, version)
assets/    fonts and branding (generated via scripts/)
scripts/   font/logo generation helpers
```

| Module | Role |
|--------|------|
| `usb_link.*` | USB-CDC framing, command parsing, host-liveness |
| `ui_sign.cpp` | Review screen, hold-to-sign, signed response |
| `tx_signer.cpp` | Parse, validate, serialize, sign (sign-what-you-see) |
| `wallet.cpp` | Encrypted seed storage, PIN unlock, ECDSA signing |

## Companion app

Build transactions online, send over USB, review on the device, broadcast the
signed blob from your PC:

**[faraday-electron-app](https://github.com/faradayXRPL/faraday-electron-app)**

## Security

Report vulnerabilities privately — see [SECURITY.md](SECURITY.md).
Do **not** open public issues for seed extraction, PIN bypass, or USB
protocol confusion.

## License

[PolyForm Noncommercial License 1.0.0](LICENSE) — Copyright (c) 2026 Faraday.

Public source for **non-commercial** use. Commercial use:
[faradayxrpl@pm.me](mailto:faradayxrpl@pm.me).
