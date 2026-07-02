// SPDX-License-Identifier: ISC
/*
 * M5Stack Cardputer (StampS3, ESP32-S3 N8R8) board bring-up: SPI3 bus for the
 * ST7789 panel and the direct GPIO matrix keyboard. No switched power rail and
 * (for now) no microSD. Compiled only for -DBOARD_CARDPUTER.
 *
 * Keyboard: a 74HC138 3-to-8 decoder driven by three GPIOs selects one of the
 * eight column groups; seven GPIOs read the rows (active-low). Each (select,
 * row) pair maps to a cell in a 4x14 grid. Ported from the M5Cardputer demo.
 */
#ifdef BOARD_CARDPUTER

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"

#include "board.h"

static const char *TAG = "board-cardputer";

/* Panel SPI bus (see board_config.h for the ST7789 pins/host). */
#define CARD_SPI_SCK 36
#define CARD_SPI_MOSI 35

/* Keyboard matrix. */
static const int kb_out[3] = { 8, 9, 11 }; /* 74HC138 select lines */
static const int kb_in[7] = { 13, 15, 3, 4, 5, 6, 7 }; /* row reads */

/* Column index for input bit j: x2 when select<=3, x1 when select>3. */
static const uint8_t kb_x1[7] = { 0, 2, 4, 6, 8, 10, 12 };
static const uint8_t kb_x2[7] = { 1, 3, 5, 7, 9, 11, 13 };

/* 4x14 keymap: base and shifted ASCII (0 = modifier / no output). */
static const char kb_base[4][14] = {
	{ '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b' },
	{ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\' },
	{ 0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\n' },
	{ 0, 0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' ' },
};
static const char kb_shift[4][14] = {
	{ '~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b' },
	{ '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '|' },
	{ 0, 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '\n' },
	{ 0, 0, 0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', ' ' },
};

/* shift key sits at grid (x=1, y=2). */
#define KB_SHIFT_X 1
#define KB_SHIFT_Y 2

static void
kb_set_select(uint8_t v)
{
	gpio_set_level(kb_out[0], v & 1);
	gpio_set_level(kb_out[1], (v >> 1) & 1);
	gpio_set_level(kb_out[2], (v >> 2) & 1);
}

static void
board_spi_init(void)
{
	spi_bus_config_t bus = {
		.mosi_io_num = CARD_SPI_MOSI,
		.miso_io_num = -1,
		.sclk_io_num = CARD_SPI_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 240 * 16 * 2, /* one text row, generous */
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
}

static void
keyboard_init(void)
{
	gpio_config_t out = {
		.pin_bit_mask = (1ULL << kb_out[0]) | (1ULL << kb_out[1]) |
		    (1ULL << kb_out[2]),
		.mode = GPIO_MODE_OUTPUT,
	};
	gpio_config(&out);
	uint64_t inmask = 0;
	for (int i = 0; i < 7; i++)
		inmask |= 1ULL << kb_in[i];
	gpio_config_t in = {
		.pin_bit_mask = inmask,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config(&in);
	kb_set_select(0);
	ESP_LOGI(TAG, "keyboard ready (GPIO matrix)");
}

void
board_init(void)
{
	board_spi_init();
	keyboard_init();
}

/*
 * Scan the matrix once. Reports one character per fresh keypress (a held key
 * does not repeat), applying shift. Returns 0 when nothing new is pressed.
 */
int
keyboard_read(void)
{
	static int last_x = -1, last_y = -1;
	int px[8], py[8], n = 0;
	bool shift = false;

	for (int i = 0; i < 8 && n < 8; i++) {
		kb_set_select((uint8_t)i);
		esp_rom_delay_us(3); /* let the 74HC138 + inputs settle */
		for (int j = 0; j < 7; j++) {
			if (gpio_get_level(kb_in[j]) != 0)
				continue; /* active low: high == not pressed */
			int x = (i > 3) ? kb_x1[j] : kb_x2[j];
			int y = 3 - ((i > 3) ? (i - 4) : i);
			if (x == KB_SHIFT_X && y == KB_SHIFT_Y)
				shift = true;
			if (n < 8) {
				px[n] = x;
				py[n] = y;
				n++;
			}
		}
	}

	/* First pressed cell that maps to a character. */
	int cx = -1, cy = -1;
	char ch = 0;
	for (int k = 0; k < n; k++) {
		char c = shift ? kb_shift[py[k]][px[k]] : kb_base[py[k]][px[k]];
		if (c != 0) {
			cx = px[k];
			cy = py[k];
			ch = c;
			break;
		}
	}

	if (ch == 0) { /* only modifiers or nothing held */
		last_x = last_y = -1;
		return 0;
	}
	if (cx == last_x && cy == last_y)
		return 0; /* same key still held: no repeat */
	last_x = cx;
	last_y = cy;
	return (unsigned char)ch;
}

#endif /* BOARD_CARDPUTER */
