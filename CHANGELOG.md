# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/HarmEllis/grocy-mealie-scanner/releases/tag/v0.0.1
