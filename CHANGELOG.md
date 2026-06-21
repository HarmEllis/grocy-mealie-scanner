# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-06-21

This release makes OTA updates easier to recover and verify from the device
itself, while tightening up settings-screen behavior and layout for longer
localized strings.

### Added

- Manual "check for updates" action in settings, so users can trigger an OTA
  release check without waiting for the periodic background check.

### Fixed

- Increased OTA HTTP receive buffers so GitHub release downloads complete
  reliably.
- Preserved the settings scroll position across UI rebuilds, including after an
  update check changes settings-screen state.
- Prevented longer translated UI labels and product quantity/unit text from
  overlapping or overflowing their containers.

## [0.1.0] - 2026-06-21

This release adds automatic over-the-air firmware updates and a much sturdier
connectivity and provisioning story: the scanner no longer pretends to be
online, supports custom API TLS trust, and can re-enter setup without a factory
reset.

### Added

- Automatic OTA firmware updates from GitHub Releases: the device checks the
  latest signed release, semver-compares it against the running version,
  prompts the user, and installs to the inactive OTA slot with rollback. The
  check runs after SNTP time-sync (so TLS validation has a valid clock) and
  then periodically (24h default) to stay under the unauthenticated rate limit.
- Custom API TLS trust in the setup portal: a CA certificate (PEM) field and a
  "trust any certificate (API only)" option. OTA keeps strict verification
  against the bundled Mozilla roots regardless of the API setting.
- Re-enter the provisioning portal without a factory reset, from the settings
  screen ("WiFi & API setup") or a short BOOT press, with WiFi/API values
  pre-filled and the rest of the config (including touch calibration) preserved.
- Signed release firmware images in CI and OTA signing documentation
  (`docs/OTA-SIGNING.md`).

### Changed

- The idle screen is now gated on a real connection. A sticky connection-error
  screen with tap and auto-retry plus a shortcut into setup replaces the old
  dismissable boot error, so idle is only reached once WiFi is up and the API
  ping succeeds.
- The GM67 scanner is held off whenever the device is not connected to the API;
  the connection-error screen and setup portal keep the aiming LED dark, and
  the scanner is gated at boot to match.
- mbedTLS allocations are routed to the octal PSRAM, giving the API and OTA TLS
  paths more headroom.

### Fixed

- "Trust any certificate (API only)" now overrides a stored CA PEM, so a CA
  whose extensions mbedTLS cannot parse no longer leaves the API unreachable.
- Fixed the API TLS handshake failing with `MBEDTLS_ERR_SSL_ALLOC_FAILED` once
  WiFi and LVGL were up, by allocating the TLS record buffers from PSRAM.

## [0.0.1] - 2026-06-20

Initial public firmware release for the Grocy-Mealie Scanner: an ESP32-S3
touchscreen barcode station with GM67 scan-engine support, a hosted web
flasher, and the first device API contract for grocy-mealie-sync.

### Added

- ESP-IDF firmware for the Freenove ESP32-S3 2.8" touch display and GM67
  barcode scanner, including WiFi provisioning, authenticated device API
  calls, factory reset, and HTTPS certificate validation.
- LVGL touchscreen flow for known products, unknown barcode linking,
  OpenFoodFacts-backed product creation, on-device search, settings, screen
  sleep, and touch calibration.
- Runtime GM67 scanner controls for scanning, status feedback, beep, LED, and
  command-mode handling, plus setup documentation for QR-code based scanner
  configuration.
- Offline demo mode with canned products and BOOT-button scenario cycling.
- ESP Web Tools flasher, GitHub Pages deployment workflow, CI firmware build,
  OTA slot-size checks, devcontainer setup, board notes, and the v1 device API
  contract.

### Fixed

- Hardened provisioning and API parsing by rejecting oversized or malformed
  WiFi setup requests, URL truncation, invalid barcode lengths, stale timeout
  events, and mismatched create/link product response shapes.

[Unreleased]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.0.1...v0.1.0
[0.0.1]: https://github.com/HarmEllis/grocy-mealie-scanner/releases/tag/v0.0.1
