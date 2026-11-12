idf_component_register(
    SRCS
        "main.c"
        "drivers/lcd_st7789.c"
        "drivers/eth_w5500.c"
        "drivers/twai_can.c"
        "drivers/encoder.c"
        "services/display.c"
        "services/network.c"
        "services/can.c"
        "services/can_monitor.c"
        "services/ui.c"
        "services/clock.c"
        "services/settings.c"
        "services/usb_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd driver esp_timer freertos esp_system
        esp_eth esp_netif esp_event nvs_flash
        esp_driver_twai esp_driver_ledc esp_driver_gpio esp_driver_usb_serial_jtag
        lwip
        knob button
        ds3231 i2cdev)
