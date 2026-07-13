# FARADAY USB Signer - Firmware v0.2

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
| Signing | Sign-what-you-see: one validated transaction object drives review and signing |
| Nonces | Hedged RFC6979 ECDSA nonces: key+message deterministic base mixed with fresh entropy |
| Storage | Production profile enables Secure Boot v2 + Flash Encryption; NVS is marked encrypted |

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
> [faraday-v0.2](https://github.com/faradayXRPL) at
> `firmware/faraday-usb-signer-v0.2`.

## v0.2 security changes

- JSON is parsed and validated once into the current reviewed transaction. The
  hold-to-sign path signs only that cached reviewed object, guarded by a payload
  SHA-256 match, so the display path and signing path cannot drift through a
  second deserialize.
- The review screen lists every user-supplied field that the serializer can
  write. Serialization refuses to emit a field unless it is present in the
  review field set.
- Nested amount/asset objects are allowlisted too. Optional fields with the
  wrong type are rejected instead of silently ignored.
- `Paths` is accepted only when its canonical JSON value fits fully on the
  device review field list; oversized path sets are rejected instead of being
  signed unseen.
- ECDSA signing uses hedged RFC6979 nonces. The nonce DRBG is seeded from the
  private key and message hash, with fresh device entropy mixed in before each
  signature.
- Wallet seed, salt, GCM nonce, and mbedTLS blinding RNG use an explicit entropy
  bootstrap based on `bootloader_random_enable()` plus hashed hardware RNG and
  timing jitter samples.

## Production security build

`sdkconfig.release.defaults` is the production profile. It enables Secure Boot
v2, Flash Encryption in release mode, AES-XTS-256, app flash-encryption checks,
and disables insecure ROM download mode. The local partition table
`partitions_faraday_secure.csv` marks the NVS partition as `encrypted`, because
the sealed seed blob lives in NVS.

Build the normal development image:

```bash
pio run -e esp32-s3-touch-lcd-3p5c
```

Build the production security image:

```bash
pio run -e esp32-s3-touch-lcd-3p5c-release
```

Production flashing is a manufacturing step, not a casual upload. Generate and
store the RSA-3072 Secure Boot private key outside the repository, keep
`secure_boot_signing_key.pem` out of source control, follow the ESP-IDF Secure
Boot v2 and Flash Encryption first-boot flow, and verify on serial logs that
Secure Boot and Flash Encryption are enabled before any device leaves the
factory. First boot burns irreversible eFuses.

## Entropy verification

Before public release, verify entropy on real air-gapped ESP32-S3 hardware:

- Cold power-cycle at least 100 devices or 100 boots of the same device.
- Capture the first diagnostic seed/RNG sample before wallet creation in a
  non-production diagnostic build only.
- Check for repeats and cross-boot correlation.
- Run NIST SP 800-90B min-entropy estimation on raw collected source samples.
- Run an SP 800-22 smoke subset on conditioned output: monobit, runs, serial.

The helper `scripts/entropy_cold_boot_check.py` collects one disabled-by-default
`ENTROPY_DIAG` sample per manual cold boot and flags exact duplicates in the CSV.

Do not enable entropy diagnostics in production builds and do not publish raw
seed material from a real wallet.

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
