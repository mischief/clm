// SPDX-License-Identifier: ISC
/*
 * Per-board configuration, selected at build time via -DBOARD_TDECK or
 * -DBOARD_CARDPUTER (see platformio.ini). Holds the ST7789 pin map, panel
 * geometry/orientation, and feature flags the shared firmware keys off.
 */
#ifndef CLM_BOARD_CONFIG_H
#define CLM_BOARD_CONFIG_H

#include "driver/spi_common.h"

#if defined(BOARD_TDECK)

#define BOARD_NAME "t-deck"
#define LCD_HOST SPI2_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_CS 12
#define LCD_DC 11
#define LCD_BL 42
#define LCD_RST (-1) /* no dedicated reset line */
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define LCD_INVERT 1
#define LCD_SWAP_XY 1
#define LCD_MIRROR_X 1
#define LCD_MIRROR_Y 0
#define LCD_GAP_X 0
#define LCD_GAP_Y 0
#define BOARD_HAS_SD 1

#elif defined(BOARD_CARDPUTER)

#define BOARD_NAME "cardputer"
#define LCD_HOST SPI3_HOST
#define LCD_H_RES 240
#define LCD_V_RES 135
#define LCD_CS 37
#define LCD_DC 34
#define LCD_BL 38
#define LCD_RST 33
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define LCD_INVERT 1
#define LCD_SWAP_XY 1
#define LCD_MIRROR_X 1
#define LCD_MIRROR_Y 0
/* 135-wide panel in the ST7789 240x320 RAM. M5GFX's native (rotation 0) offsets
 * are 52/40; LovyanGFX's setRotation(1) transforms them for our landscape
 * (swap_xy + mirror_x) orientation to colstart=40, rowstart=240-(135+52)=53. */
#define LCD_GAP_X 40
#define LCD_GAP_Y 53
#define BOARD_HAS_SD 0

#else
#error "no board selected: define BOARD_TDECK or BOARD_CARDPUTER"
#endif

#endif /* CLM_BOARD_CONFIG_H */
