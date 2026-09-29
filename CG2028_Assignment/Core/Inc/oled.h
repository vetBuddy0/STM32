/******************************************************************************
 * @file           : oled.h
 * @brief          : SSD1306 128x64 OLED on I2C1 (PB8/PB9), text only.
 *
 * 8 rows x 21 small characters, or 10-character double-size headlines.
 * Drawing only writes to a RAM copy of the screen; oled_service() sends one
 * changed row per call so it never takes long.
 ******************************************************************************/
#ifndef OLED_H
#define OLED_H

#include <stdint.h>

/* One text row = one SSD1306 page (8 px tall) */
#define OLED_ROWS               8
#define OLED_COLS              21   /* 6 px per character */
#define OLED_BIG_COLS          10   /* 12 px per character */

/* One row is ~2.9 ms at 400 kHz */
#define OLED_ROWS_PER_SERVICE   1

/* Fine to call with no display attached, everything else then does nothing */
void oled_init(void);

void oled_clear(void);

/* Clears the rest of the row after the text */
void oled_text(uint8_t row, uint8_t col, const char *text);

/* Double height, uses row and row + 1 */
void oled_big(uint8_t row, const char *text);

/* "[####------] label" */
void oled_bar(uint8_t row, uint8_t percent, const char *label);

/* Main loop, once per pass */
void oled_service(void);

/* Sends all changed rows now (blocking) - start-up only */
void oled_flush(void);

uint8_t oled_is_present(void);

/* 0x78 or 0x7A (8-bit write address), 0 if nothing answered */
uint8_t oled_address(void);

uint8_t oled_probe(uint8_t wire_addr);

#endif /* OLED_H */
