idf_component_register(SRCS "main.cpp"
                            "ui.c"
                            "input.c"
                            "serial_input.c"
                            "usb_protocol.c"
                            "wifi_sniffer.c"
                            "ble_spam.c"
                            "beacon.c"
                            "portal.c"
                            "deauth.c"
                            "handshake.c"
                            "sd_card.c"
                            "status_led.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "../webpage.html"
                    REQUIRES esp_wifi bt driver fatfs sdmmc esp_netif esp_event
                             nvs_flash esp_driver_ledc esp_http_server lwip
                             esp_lcd
                             lvgl
                             esp_driver_usb_serial_jtag
                             LovyanGFX)