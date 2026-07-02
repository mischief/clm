// SPDX-License-Identifier: ISC
/*
 * Minimal TTY-style text console on the T-Deck's ST7789 (320x240) via esp_lcd.
 * A fixed character grid (8x16 font -> 40x15), scroll-on-newline, per-cell color.
 * No graphics framework -- just a VGA-like console for the agent's chat output.
 *
 * Assumes the peripheral power rail (GPIO10) is on and the shared SPI bus is
 * already initialized (see board_spi_init in main.c).
 */
#ifndef CLM_DISPLAY_H
#define CLM_DISPLAY_H

#include <stdint.h>

/* RGB565 palette. */
#define DISP_BLACK  0x0000
#define DISP_WHITE  0xFFFF
#define DISP_GREEN  0x07E0
#define DISP_CYAN   0x07FF
#define DISP_YELLOW 0xFFE0
#define DISP_RED    0xF800
#define DISP_GRAY   0x8410
#define DISP_BLUE   0x001F

int display_init(void);

void console_set_color(uint16_t fg);
void console_putc(char c);
void console_puts(const char *s);
void console_backspace(void); /* erase the char left of the cursor */
void console_flush(void); /* push dirty rows to the panel */
void console_clear(void);
void console_screenshot(void); /* dump the grid as a PPM over the serial link */

#endif /* CLM_DISPLAY_H */
