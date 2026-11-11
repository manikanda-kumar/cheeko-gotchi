# cheeko_runtime: implements sdk/include/cheeko.h on the Cheeko Gotchi board.
idf_component_register(
    SRCS "cheeko_runtime.cc" "cheeko_board.c" "cheeko_link.cc"
    INCLUDE_DIRS "include" "../../sdk/include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES lvgl esp_lvgl_port
    PRIV_REQUIRES driver esp_driver_usb_serial_jtag mbedtls esp_lcd esp_timer nvs_flash esp_lcd_jd9853
)
