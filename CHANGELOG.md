# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.5.0] - 2026-07-01

On-device UX release: a large-key alphabetical keyboard replaces the cramped
default, product actions gain a quantity picker with a before→after preview,
the product page shows how many are already on the shopping list, and unknown
barcodes can always be turned into a product.

### Added

- Large-key A-Z on-screen keyboard shared by product search and product
  creation, replacing LVGL's cramped default QWERTY. Equal-width keys, a
  control bar (hide / 123 / #+= / space / confirm) with a per-screen confirm
  label, and a hide key that collapses the keyboard so the search result list
  grows to fill the space.
- Quantity picker for product actions: tapping a purchase/open/consume/shopping
  tile opens a confirm screen with a tappable predicted "before → after"
  preview and a ~3 s auto-confirm countdown (amount 1). Tapping the preview
  reveals a 1-9 keypad and a separate Confirm button; invalid amounts (consume
  above stock, open above stock-opened) disable Confirm and skip auto-confirm.
- Shopping-list count card: a fourth "LIST" stat on the product page shows how
  many of the product are already on the shopping list, fed by a new
  best-effort `shoppingListAmount` field on the device product (`apiVersion 3`).
- Back button (top-left chevron) on the search screen, returning to idle and
  matching the product screen's convention.
- The search and product-name fields now show a blinking amber block cursor so
  it's clear where typed characters will land.

### Changed

- "Create product" is now always offered on the unknown-barcode screen. It was
  previously gated on Open Food Facts returning a name, leaving barcodes OFF
  didn't recognise with only the search option. The OFF name pre-fills the
  field when present; otherwise the create screen opens empty.
- WiFi power-save (modem sleep) now defaults to **off** for lower HTTP latency
  and more reliable connections. It can still be enabled from the settings
  screen to save power. Existing devices that never toggled the setting also
  pick up the new default.
- The device API capability version advertised on `/ping` is now `3`
  (`shoppingListAmount` on device products); the demo client reports 3 to match.

### Fixed

- Product fetches from a search pick or last-scan tap now show the "Loading"
  spinner instead of the "Saving" one, since they only read product info.

## [0.4.1] - 2026-06-28

Documentation fix release.

### Fixed

- Hardware docs (`README.md`, `firmware/BOARD_NOTES.md`) now correctly state
  that the GM67 scanner's VCC must be powered from **5 V** (the board's 5 V /
  USB VBUS rail), not the 3V3 header pin. The module does not run reliably on
  3.3 V. Its UART lines remain TTL 3.3 V-tolerant, so the IO21/IO14 signal
  wiring is unchanged.

## [0.4.0] - 2026-06-27

WiFi connectivity release: this hardens provisioning and station setup so the
device reliably joins real-world networks (mesh, multi-AP, and routers stuck on
the upper 2.4 GHz channels), makes the captive-portal SoftAP discoverable on the
ESP32-S3, and adds a serial-free PHY recalibration recovery path.

### Added

- Per-device WiFi country override in the captive portal, persisted in NVS, so a
  device can be pointed at a stricter regulatory domain (e.g. set `NL` to force
  active scanning on channels 1-13 for a router parked on channel 13). Empty
  falls back to the build default.
- "Recalibrate WiFi" action on both the settings screen and the connection-error
  screen that wipes the stored PHY calibration data and reboots, recovering from
  corrupt calibration without serial access.

### Changed

- The regulatory domain now defaults to the world-safe `"01"` (legal anywhere),
  with 802.11d adaptation plus the per-device override covering edge-case
  routers.
- The station now performs an all-channel scan sorted by signal strength and
  connects to the strongest matching AP instead of the first one advertising the
  SSID, fixing repeated latching onto a weak or flaky AP in mesh, multi-AP, and
  repeater networks.
- The station advertises WPA3-SAE/PMF capability (`pmf_cfg.capable`,
  `sae_pwe_h2e`) and logs the disconnect reason code to aid diagnosis.

### Fixed

- The captive-portal SoftAP is now discoverable on the ESP32-S3: bandwidth
  (HT20) and 11b/g protocol are set before `esp_wifi_start()`, and the
  regulatory country code is applied before start so all channels are usable
  (works around ESP-IDF #13508).

## [0.3.0] - 2026-06-22

Idle-screen usability release: product search and last-scan shortcuts let you
reach any product page without scanning a barcode, scan lookups are more
resilient to transient network issues, and a new WiFi power-save toggle trades
battery for lower latency.

### Added

- On-device product search from the idle screen, gated on the server's
  `apiVersion >= 2` capability: a search icon on the status bar opens the
  keyboard search, and picking a result shows the product page without linking
  a barcode.
- `GET /api/device/v1/products/{id}` endpoint in the device API contract for
  fetching a single product by id.
- Tappable last-scan shortcut on the idle screen: the last-scanned product name
  in the footer is highlighted in amber and tappable when the server supports
  `apiVersion >= 2`, re-opening the product page for quick repeat actions
  (purchase, consume, open, shopping list) without rescanning the barcode.
- WiFi power-save toggle in settings: disabling modem sleep reduces HTTP
  latency at the cost of higher power draw, helpful on networks where the
  ESP32 WiFi sleep handshake adds noticeable lag to scan lookups.

### Changed

- Scan lookups now retry transport-level failures (TLS, TCP, DNS) up to
  3 times with escalating back-off (300 ms, 800 ms), only when the failed
  attempt completed quickly (< 2.5 s). Application errors (4xx/5xx) are never
  retried.
- Transport failures on scan now route to the sticky connection-error screen
  (with tap-to-retry and auto-retry) instead of a dismissable one-shot error,
  matching the boot connection-error behaviour.

### Fixed

- Scan error handling now distinguishes transport failures from server errors,
  so network-level issues surface the correct retry UI instead of a dead-end
  error screen.

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

[Unreleased]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/HarmEllis/grocy-mealie-scanner/compare/v0.0.1...v0.1.0
[0.0.1]: https://github.com/HarmEllis/grocy-mealie-scanner/releases/tag/v0.0.1
