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
the native orientation already is 240 wide × 320 tall). The panel X axis is
mirrored in software to match the board's physical orientation. Touch uses
its native X orientation.

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

GM67 defaults: 9600 baud 8N1, SSI/plain-text output. At boot the firmware now
**actively configures the module** over UART1 (serial command protocol,
Appendix 6 of the GM67 manual): continuous scan, Code-only payload, CR LF
terminator, STX/ETX off, raw (non-packet) output, good-read beep, 3 s scan
time. See `docs/GM67-IMPROVEMENT-PLAN.md` for the design and exact byte
sequences. This means a unit does **not** have to be pre-configured with setup
barcodes — *provided it is reachable on TTL serial at 9600 8N1*. Configuration
is best-effort and fail-open: a module that does not answer (wrong baud,
USB-KBW mode, older firmware) falls through to passive reading, so the read
path is never bricked; for such a unit the setup-barcode bootstrap from the
manual is still needed for first contact. The trigger line stays unwired.
IO3/IO2 stay free (future presence sensor; the buzzer/LED are driven over
serial, so they need no extra GPIO).

The GM67 serial protocol lives in `main/gm67_proto.{c,h}` as a pure,
ESP-IDF-free layer (command tables + the reply/scan stream demultiplexer) so
it is unit-tested on the host — run `firmware/test/run.sh` (plain `cc`, no
devcontainer needed).

## Other on-board peripherals (unused for now)

| Peripheral | GPIOs |
|------------|-------|
| microSD (SDMMC 4-bit) | CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47 |
| ES8311 codec + SC8002B amp (I2S) | MCK 4, BCK 5, DIN 6, WS 7, DOUT 8 |
| Microphone | analog, into ES8311 |
| Battery ADC | 9 (×2 divider) |
| WS2812 RGB LED | 42 (5 V supply) |
| BOOT key | 0 |

The WS2812 (GPIO42) is now the **scan-result indicator** — see `main/status_led.c`:
a brief green/amber/coral flash (found / not-found / error) driven over RMT via
the `espressif/led_strip` component, dimmed and auto-cleared by an esp_timer
one-shot. It is user-toggleable from the settings screen (default on). The
WS2812 is chosen over the GM67's own LED/buzzer for the *result* cue because the
outcome is known only after the network round-trip (see
`docs/GM67-IMPROVEMENT-PLAN.md` §4.2); the GM67's immediate decode beep stays the
audio channel and is the other settings toggle. The on-board speaker/codec is
still out of scope for v1.

## Design → LVGL notes

The UI follows `design/Grocy-Mealie-Scanner_variant-a.html` (1:1 240×320).
Deviations agreed with the maintainer:

- The colour-flash radial gradient + ring pulse are reduced to a static
  colour badge because large translucent transforms overload software
  rendering on the ESP32-S3.
- The flash screen is shown only after the API call succeeds and shows the
  real old→new amounts; a "saving…" state bridges tap→response.
- Fonts: LVGL's bitmap Montserrat fonts render the common UI path. The bundled
  OFL Montserrat TTF is attached as a Tiny TTF fallback at each used size, so
  Dutch diacritics and accented product names render without making every
  glyph use the slower runtime rasterizer.
- Extra states not in the design (provisioning, offline, OTA progress,
  link/create/search flow for unknown barcodes) reuse the same palette.
- Settings screen (`design/Grocy-Mealie-Scanner_variant-a.html`, settings
  state): the design's SVG gear/back icons and custom pill toggles are rendered
  with the built-in LVGL symbol font (`LV_SYMBOL_SETTINGS`, `LV_SYMBOL_LEFT`)
  and `lv_switch`, matching the rest of the UI. The gear sits on the idle status
  bar; the product screen gains a back chevron there. Both feedback toggles
  (scanner beep, status light), the English/Dutch language choice, and the
  screen-timeout preset (30/60/120/300 s / Never, default 60 s) reach the
  screen from idle and persist to NVS.  When the idle timeout fires, backlight
  and panel are powered off and scan forwarding is gated in software (the GM67
  stays in continuous-trigger mode; no PARAM_SEND laser-stop command is sent).

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
