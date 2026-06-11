/* Pin map for the Freenove ESP32-S3 Display 2.8" (FNK0104AB).
 * See BOARD_NOTES.md for sources and quirks. */
#pragma once

/* LCD — ILI9341, 4-wire SPI (SPI2), BGR order, inversion ON */
#define BOARD_LCD_SPI_HOST     SPI2_HOST
#define BOARD_LCD_PIN_MOSI     11
#define BOARD_LCD_PIN_MISO     13
#define BOARD_LCD_PIN_SCLK     12
#define BOARD_LCD_PIN_CS       10
#define BOARD_LCD_PIN_DC       46
#define BOARD_LCD_PIN_BL       45   /* high = on */
#define BOARD_LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define BOARD_LCD_H_RES        240
#define BOARD_LCD_V_RES        320

/* Touch — FT6336U on I2C0, addr 0x38 (FT5x06-compatible) */
#define BOARD_TOUCH_I2C_PORT   0
#define BOARD_TOUCH_PIN_SDA    16
#define BOARD_TOUCH_PIN_SCL    15
#define BOARD_TOUCH_PIN_INT    17
#define BOARD_TOUCH_PIN_RST    18

/* GM67 barcode scanner — UART1 on the Expanded-IO header */
#define BOARD_GM67_UART_NUM    1
#define BOARD_GM67_PIN_TX      21   /* ESP TX → GM67 RX */
#define BOARD_GM67_PIN_RX      14   /* ESP RX ← GM67 TX */
#define BOARD_GM67_BAUD        9600

/* Misc */
#define BOARD_PIN_BOOT_KEY     0
#define BOARD_PIN_RGB_LED      42
