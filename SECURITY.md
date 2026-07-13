# Security — FARADAY USB Signer firmware

This firmware runs on **air-gapped hardware**. Treat it as security-sensitive.

## Reporting a vulnerability

**Do not** open public GitHub issues for security bugs.

Email **[faradayxrpl@pm.me](mailto:faradayxrpl@pm.me)** with:

- Description and impact
- Firmware version (`FW_VERSION` in `include/config.h`)
- Steps to reproduce (transaction type, USB sequence)
- Proof-of-concept if available

We aim to acknowledge within **72 hours**.

## In scope

- Seed extraction or PIN bypass while the device is locked
- Signing without explicit hold-to-sign confirmation
- USB protocol confusion, replay, or field substitution (breaks sign-what-you-see)
- Downgrade to unsigned or wrong-network payloads

## Out of scope

- Physical theft of an unlocked device
- Compromise of the host PC running the online companion (expected threat model)
- XRPL network or third-party wallet bugs

## Build hygiene

- Never commit device seeds, PINs, or NVS dumps
- `.pio/` and build artifacts stay local (see `.gitignore`)
- Verify reproducible builds from a clean checkout before trusting a binary

## v0.2 release hardening

- Production devices must be built with `esp32-s3-touch-lcd-3p5c-release`.
- The release profile enables Secure Boot v2 and Flash Encryption release mode.
- The NVS partition is marked `encrypted` in `partitions_faraday_secure.csv`.
- Keep `secure_boot_signing_key.pem` outside the repository and manufacturing logs.
- Do not ship firmware with `cfg::ENTROPY_DIAGNOSTICS=true`.
