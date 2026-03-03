idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_netif esp_tinyusb nvs_flash led_strip)
