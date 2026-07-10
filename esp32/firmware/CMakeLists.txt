idf_component_register(
    SRCS "main.c" "display.c" "board_tdeck.c" "board_cardputer.c"
    INCLUDE_DIRS "."
    REQUIRES libclm synth_vfs json nvs_flash esp_wifi esp_netif esp_event
             esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_spi
             esp_driver_sdspi sdmmc fatfs esp_lcd esp_driver_i2c vfs
)
