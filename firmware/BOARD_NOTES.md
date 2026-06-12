# Board notes — Freenove ESP32-S3 Display 2.8" (FNK0104AB)

Sources: Freenove `Freenove_ESP32_S3_Display` repo (TFT_eSPI setup
`FNK0104AB_2.8_240x320_ILI9341.h`, example sketches with
`FNK0104AB_2P8_240x320_ILI9341` blocks) and
`Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf`. The 3.5"/4.0" boards in
the same repo use different pins — always take the `FNK0104AB` branch of the
`#ifdef`s.

## SoC / memory

- ESP32-S3R8: 8 MB **octal** PSRAM (`CONFIG_SPIRAM_MODE_OCT`).
- 25VQ128 flash: 16 MB. Partition table uses 3 MB + 3 MB OTA slots.

## LCD — ILI9341, 4-wire SPI

| Signal | GPIO | Notes |
|--------|------|-------|
| MOSI | 11 | |
| MISO | 13 | |
| SCLK | 12 | 40 MHz works |
| CS | 10 | |
| DC (RS) | 46 | |
| RST | — | tied to board RESET |
| Backlight | 45 | high = on (BSS138 driver) |

Panel quirks (from the TFT_eSPI setup): **BGR** colour order and
**inversion ON**. Resolution 240×320; we run portrait (no rotation needed,
the native orientation already is 240 wide × 320 tall).

## Touch — FT6336U, I2C addr 0x38

| Signal | GPIO |
|--------|------|
| SDA | 16 |
| SCL | 15 |
| INT | 17 |
| RST | 18 |

The I2C bus is shared with the ES8311 audio codec (0x18) and the external
IIC header. FT6336U is FT5x06-protocol compatible
(`esp_lcd_touch_ft5x06` driver works).

## GM67 barcode scanner — UART1 on the Expanded-IO header

The Expanded-IO header exposes IO21, IO14, IO3, IO2 (+3V3/GND). The UART
header exposes UART0 (TXD0/RXD0 = GPIO43/44) which stays the log console —
do not put the GM67 there.

| GM67 pin | Connect to | ESP32-S3 role |
|----------|------------|---------------|
| RX | IO21 | UART1 TX |
| TX | IO14 | UART1 RX |
| VCC | 3V3 | |
| GND | GND | |

GM67 defaults: 9600 baud 8N1, SSI/plain-text output. The firmware does **not**
configure the scanner — it only opens UART1 and reads decoded barcodes. The
module must therefore already be in **continuous (auto-sense) scan mode with
serial/TTL plain-text output at 9600 8N1**, and the trigger line is left
unwired. On most GM67 units this is the factory default; if yours ships in
host/command-trigger mode, set continuous mode once with the configuration
barcode from the GM67 manual (no firmware change needed). IO3/IO2 stay free
(candidate: GM67 buzzer/LED control or a future presence sensor).

## Other on-board peripherals (unused for now)

| Peripheral | GPIOs |
|------------|-------|
| microSD (SDMMC 4-bit) | CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47 |
| ES8311 codec + SC8002B amp (I2S) | MCK 4, BCK 5, DIN 6, WS 7, DOUT 8 |
| Microphone | analog, into ES8311 |
| Battery ADC | 9 (×2 divider) |
| WS2812 RGB LED | 42 (5 V supply) |
| BOOT key | 0 |

The WS2812 could mirror the action colour flash later; the speaker could
beep on scan. Both are out of scope for v1.

## Design → LVGL notes

The UI follows `design/Grocy-Mealie-Scanner_variant-a.html` (1:1 240×320).
Deviations agreed with the maintainer:

- The colour-flash radial gradient + ring pulse are approximated with a
  pre-rendered per-colour background image + an LVGL arc animation.
- The flash screen is shown only after the API call succeeds and shows the
  real old→new amounts; a "saving…" state bridges tap→response.
- Fonts: Plus Jakarta Sans (text) and JetBrains Mono (numbers/codes), both
  OFL, converted to LVGL bitmap fonts.
- Extra states not in the design (provisioning, offline, OTA progress,
  link/create/search flow for unknown barcodes) reuse the same palette.

Palette (from the design export):

| Token | Hex |
|-------|-----|
| device background | `#0b0b0c` |
| card | `#1e1e22` |
| text | `#ededf0` |
| dim | `#8b8b93` |
| dim2 | `#5e5e66` |
| amber (accent / idle) | `#f5c13d` |
| green (bought) | `#35c98c` |
| gold (opened) | `#f0a93a` |
| coral (consumed) | `#e8674a` |
| blue (shopping) | `#5aa0ef` |
