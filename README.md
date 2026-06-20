# grocy-mealie-scanner

A kitchen barcode scanner station for [Grocy](https://grocy.info) +
[Mealie](https://mealie.io), built on a Freenove ESP32-S3 2.8" IPS
capacitive-touch display (FNK0104AB, 240×320 portrait) and a GM67 barcode
scan engine. Scan a product, see its stock level, configured minimum and how
many are opened, then tap one of four tiles:

- **Bought** — add one to Grocy stock
- **Opened** — mark one as opened
- **Consumed** — consume one
- **Shopping** — put it on the Mealie shopping list

Unknown barcodes go through a guided flow: link the barcode to an existing
Grocy product (fuzzy-matched suggestion or on-device search) or create the
product from an OpenFoodFacts proposal with an editable name.

The device talks to a device API hosted by
[grocy-mealie-sync](https://github.com/HarmEllis/grocy-mealie-sync); see
[`docs/DEVICE-API.md`](docs/DEVICE-API.md) for the contract.

[![CI](https://github.com/HarmEllis/grocy-mealie-scanner/actions/workflows/ci.yml/badge.svg)](https://github.com/HarmEllis/grocy-mealie-scanner/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

## Contents

- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Flashing the firmware](#flashing-the-firmware)
- [Device setup and operation](#device-setup-and-operation)
- [Development](#development)
  - [Devcontainer](#devcontainer)
  - [Repository layout](#repository-layout)
  - [Firmware](#firmware)
  - [Demo mode](#demo-mode)
  - [Tests](#tests)
  - [Documentation](#documentation)
- [License](#license)

## Hardware

| Part | Details |
|------|---------|
| Board | Freenove ESP32-S3 Display 2.8" (FNK0104AB): ESP32-S3R8, 16 MB flash, 8 MB PSRAM, ILI9341 240×320 IPS, FT6336U capacitive touch |
| Scanner | GM67 barcode scan engine, wired to the Expanded-IO header (UART1: ESP TX = GPIO21 → GM67 RX, ESP RX = GPIO14 ← GM67 TX, 3V3 + GND) |

Full pin map and bring-up notes: [`firmware/BOARD_NOTES.md`](firmware/BOARD_NOTES.md).

## Quick start

No local toolchain is required to install — flash the published image and
configure the device on first boot.

1. Flash the firmware with the [web flasher](#flashing-the-firmware).
2. On first boot the touchscreen shows a QR code and a SoftAP name like
   `scanner-A4F2`. Join it and open `http://192.168.4.1` to configure WiFi,
   the grocy-mealie-sync base URL and the device token.
3. Set `DEVICE_API_TOKENS` in your grocy-mealie-sync instance to the same
   token.

The device reboots, pings the API, and lands on the idle screen ready to scan.

> **Scanner first-time setup** — before scanning barcodes, configure the GM67
> scanner with four QR codes from its manual (TTL serial, 9600 baud, continuous
> mode, CR+LF terminator). See [docs/scanner-setup.md](docs/scanner-setup.md)
> for step-by-step instructions.

## Flashing the firmware

### Web flasher (recommended)

The flasher uses the Web Serial API and runs in desktop Chrome or Edge
(Firefox, Safari and mobile browsers are unsupported).

1. Connect the ESP32-S3 over USB.
2. Open the hosted flasher at
   <https://harmellis.github.io/grocy-mealie-scanner/> (published from
   `flash-server/` by the Pages workflow), or serve it locally — see
   [Firmware](#firmware).
3. Click **Install** and select the serial port.
4. Erase the device when prompted for a fresh installation.

If the serial port does not appear, hold **BOOT**, tap **RESET**, then release
BOOT to enter ROM download mode and try again.

### Command-line flash

For a locally built image (`firmware/build/`), the four parts and their flash
offsets are:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xf000  build/ota_data_initial.bin \
  0x20000 build/grocy-mealie-scanner.bin
```

## Device setup and operation

On first boot — and after a factory reset — the display shows a WiFi QR code,
the SoftAP name and password, and `http://192.168.4.1`.

1. Scan the QR code (or join the `scanner-XXXX` network manually).
2. Open the captive portal at `http://192.168.4.1`.
3. Select English or Dutch, then enter your WiFi credentials, the
   grocy-mealie-sync base URL (for example `http://192.168.1.50:3000`) and the
   device token.
4. Save; the device reboots and connects.

API URLs may use `http://` or `https://`; HTTPS certificates are validated
against the ESP-IDF root certificate bundle.

Day-to-day use:

| Screen | Interaction |
|--------|-------------|
| Idle | Scan a barcode, or tap the gear for on-device settings (scanner beep, status light, language, screen timeout); display blanks after the configured idle time (default 60 s) — tap to wake |
| Product | Tap **Bought / Opened / Consumed / Shopping**; a coloured flash confirms |
| Unknown barcode | Link to a suggested or searched product, or create from the OpenFoodFacts proposal |

### BOOT button

| Action | Result |
|--------|--------|
| Long press (~5 s) | **Factory reset** — erases saved WiFi, API URL and token, then reboots into the setup portal |

On a [demo](#demo-mode) image a short press instead injects the next demo
scenario; the 5 s factory-reset hold still applies.

## Development

### Devcontainer

All firmware builds and flash-server commands run inside the VS Code
devcontainer in [`.devcontainer/`](.devcontainer/), which provides ESP-IDF
v5.3 and the project toolchain. Open the repository in VS Code and choose
**Reopen in Container**; do not run `idf.py` directly on the host. The exact
commands and conventions live in [AGENTS.md](AGENTS.md).

### Repository layout

```text
firmware/       ESP-IDF (v5.3) firmware, LVGL UI
  main/         Application code (state machine, UI, GM67, API client)
  test/         Host unit tests for the GM67 protocol/demux (pure C)
flash-server/   ESP Web Tools flasher (local + GitHub Pages), port 8080
docs/           Device API contract and the GM67 improvement plan
design/         Claude design export the UI follows (variant A + colour flash)
```

The server side of the device API lives in the
[grocy-mealie-sync](https://github.com/HarmEllis/grocy-mealie-sync) repo;
[`docs/DEVICE-API.md`](docs/DEVICE-API.md) is the single source of truth for
that contract.

### Firmware

Inside the devcontainer:

```bash
cd firmware
idf.py set-target esp32s3   # once, after a clean checkout
idf.py build
cd ../flash-server
bash serve.sh               # copies binaries to bins/ and serves on :8080
```

Open `http://localhost:8080` in desktop Chrome or Edge. The host browser owns
the USB connection while the devcontainer builds and serves the binaries.

Use `flash-server/watch.sh` instead of `serve.sh` to republish binaries after
each successful firmware build.

### Demo mode

`CONFIG_GMS_DEMO_MODE` builds a self-contained demo image: no WiFi, no
provisioning, no backend. The real `api_client.c` is swapped for
`api_client_demo.c`, which serves canned product fixtures and keeps stock in
RAM, so the whole UI flow — found product, actions with live before/after
counts, unknown barcode → link/create/search, error screen — can be shown
offline.

Enable it through menuconfig and build as usual:

```bash
cd firmware
idf.py menuconfig          # Grocy-Mealie scanner → [*] Demo mode
idf.py build
cd ../flash-server && bash serve.sh
```

On a demo image boot goes straight to idle. A **short BOOT press** injects the
next scenario in a fixed cycle (found product, action, unknown barcode with an
OpenFoodFacts name and suggestions, bare unknown, simulated error), so the
flow works with no GM67 module and no printed barcode. A real GM67 scan also
works — any code resolves to a fixture or a generated product. Keep
`CONFIG_GMS_DEMO_MODE` **off** for production firmware.

### Tests

The GM67 protocol and demux layer have host unit tests that need only a C
compiler — no ESP-IDF:

```bash
bash firmware/test/run.sh
```

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) builds the
esp32s3 firmware on every push and enforces the OTA partition guards, so a
green check means the release is buildable.

### Documentation

- Device API contract: [`docs/DEVICE-API.md`](docs/DEVICE-API.md)
- Firmware board and bring-up notes: [`firmware/BOARD_NOTES.md`](firmware/BOARD_NOTES.md)
- GM67 improvement plan: [`docs/GM67-IMPROVEMENT-PLAN.md`](docs/GM67-IMPROVEMENT-PLAN.md)
- OTA signing setup: [`docs/OTA-SIGNING.md`](docs/OTA-SIGNING.md)
- Release history: [`CHANGELOG.md`](CHANGELOG.md)

## License

[MIT](LICENSE)
