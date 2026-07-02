// SPDX-License-Identifier: ISC
/*
 * LilyGo T-Deck (ESP32-S3 N16R8) board bring-up: switched peripheral power
 * rail, shared SPI2 bus (display + microSD + LoRa), FAT microSD at /sd, and
 * the I2C keyboard (an ESP32-C3 at 0x55). Compiled only for -DBOARD_TDECK.
 */
#ifdef BOARD_TDECK

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "driver/i2c_master.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "board.h"

static const char *TAG = "board-tdeck";

/* T-Deck pin map, from the LilyGo reference. */
#define TDECK_POWERON_GPIO 10 /* switches the peripheral power rail */
#define TDECK_SPI_SCK 40
#define TDECK_SPI_MISO 38
#define TDECK_SPI_MOSI 41 /* shared SPI: display CS 12, SD CS 39, LoRa CS 9 */
#define TDECK_SD_CS 39
#define TDECK_TFT_CS 12 /* deselect display + radio so only SD answers */
#define TDECK_RADIO_CS 9
#define TDECK_SD_MOUNT "/sd"
#define TDECK_I2C_SDA 18
#define TDECK_I2C_SCL 8
#define TDECK_KB_ADDR 0x55 /* LilyGo keyboard (an ESP32-C3) on I2C */

static i2c_master_dev_handle_t s_kb;

/*
 * The display, keyboard, and microSD sit on a switched power rail gated by
 * GPIO10; it must be driven HIGH (and given a moment to settle) before any of
 * them respond. Miss this and the SD card simply isn't there.
 */
static void
board_power_on(void)
{
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << TDECK_POWERON_GPIO,
		.mode = GPIO_MODE_OUTPUT,
	};
	gpio_config(&io);
	gpio_set_level(TDECK_POWERON_GPIO, 1);
	vTaskDelay(pdMS_TO_TICKS(500)); /* peripherals (and the kb C3) boot */
}

/*
 * Bring up the SPI bus shared by the SD card, display, and LoRa radio. Drive
 * every chip-select HIGH (deselected) first so no peripheral interferes, then
 * init the bus once. max_transfer_sz covers a full display text row.
 */
static void
board_spi_init(void)
{
	gpio_config_t cs = {
		.pin_bit_mask = (1ULL << TDECK_SD_CS) | (1ULL << TDECK_TFT_CS) |
		    (1ULL << TDECK_RADIO_CS),
		.mode = GPIO_MODE_OUTPUT,
	};
	gpio_config(&cs);
	gpio_set_level(TDECK_SD_CS, 1);
	gpio_set_level(TDECK_TFT_CS, 1);
	gpio_set_level(TDECK_RADIO_CS, 1);

	spi_bus_config_t bus = {
		.mosi_io_num = TDECK_SPI_MOSI,
		.miso_io_num = TDECK_SPI_MISO,
		.sclk_io_num = TDECK_SPI_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 320 * 16 * 2,
	};
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
}

/* Mount the microSD (FAT) at /sd. Non-fatal: the agent still runs, the file
 * tools just have nothing under /sd. */
static void
sd_mount(void)
{
	esp_err_t err;
	sdmmc_card_t *card = NULL;

	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = SPI2_HOST;
	host.max_freq_khz = 800; /* conservative; the shared bus is fussy */

	sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot.gpio_cs = TDECK_SD_CS;
	slot.host_id = SPI2_HOST;

	esp_vfs_fat_sdmmc_mount_config_t mcfg = {
		.format_if_mount_failed = false, /* never reformat the user's card */
		.max_files = 5,
		.allocation_unit_size = 16 * 1024,
	};

	err = esp_vfs_fat_sdspi_mount(TDECK_SD_MOUNT, &host, &slot, &mcfg, &card);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "SD: mount failed: %s (no card, or wiring/power?)",
		    esp_err_to_name(err));
		return;
	}
	ESP_LOGI(TAG, "SD mounted at %s: %lluMB", TDECK_SD_MOUNT,
	    ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024));
}

static void
keyboard_init(void)
{
	i2c_master_bus_config_t bus = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = TDECK_I2C_SDA,
		.scl_io_num = TDECK_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bushandle;
	if (i2c_new_master_bus(&bus, &bushandle) != ESP_OK) {
		ESP_LOGW(TAG, "keyboard: I2C bus init failed");
		return;
	}
	i2c_device_config_t dev = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = TDECK_KB_ADDR,
		.scl_speed_hz = 100000,
	};
	if (i2c_master_bus_add_device(bushandle, &dev, &s_kb) != ESP_OK) {
		ESP_LOGW(TAG, "keyboard: add device failed");
		s_kb = NULL;
		return;
	}
	ESP_LOGI(TAG, "keyboard ready (I2C 0x%02x)", TDECK_KB_ADDR);
}

void
board_init(void)
{
	board_power_on();
	board_spi_init();
	sd_mount();
	keyboard_init();
}

/* A 1-byte read returns the ASCII of the last keypress, or 0 if none. */
int
keyboard_read(void)
{
	uint8_t v = 0;
	if (s_kb == NULL)
		return 0;
	if (i2c_master_receive(s_kb, &v, 1, 20) != ESP_OK)
		return 0;
	return v;
}

#endif /* BOARD_TDECK */
