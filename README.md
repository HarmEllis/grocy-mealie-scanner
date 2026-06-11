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

## Hardware

| Part | Details |
|------|---------|
| Board | Freenove ESP32-S3 Display 2.8" (FNK0104AB): ESP32-S3R8, 16 MB flash, 8 MB PSRAM, ILI9341 240×320 IPS, FT6336U capacitive touch |
| Scanner | GM67 barcode scan engine, wired to the Expanded-IO header (UART1: ESP TX = GPIO21 → GM67 RX, ESP RX = GPIO14 ← GM67 TX, 3V3 + GND) |

Full pin map and bring-up notes: [`firmware/BOARD_NOTES.md`](firmware/BOARD_NOTES.md).

## Getting started

1. Flash the firmware with the web flasher (`flash-server/`, or the hosted
   GitHub Pages flasher once released).
2. On first boot the touchscreen shows a QR code and a SoftAP name like
   `scanner-A4F2`. Join it and open `http://192.168.4.1` to configure WiFi,
   the grocy-mealie-sync base URL and the device token.
3. Set `DEVICE_API_TOKENS` in your grocy-mealie-sync instance to the same
   token.

## Development

Everything builds inside the devcontainer (`.devcontainer/`); see
[AGENTS.md](AGENTS.md) for the exact commands.

```
firmware/       ESP-IDF (v5.3) firmware, LVGL UI
flash-server/   ESP Web Tools flasher, port 8080
docs/           Device API contract, decisions
design/         Claude design export the UI follows (variant A + colour flash)
```

## License

[MIT](LICENSE)
