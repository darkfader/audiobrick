idf_component_register(SRCS "main.c" "dac.c" "tone.c" "net.c" "ota_http.c" "limits.c"
                            "media.c" "stream.c" "clip.c" "storage.c" "clips_http.c" "eq.c" "eq_http.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "index.html"
                       PRIV_REQUIRES app_update esp_driver_gpio esp_driver_i2c esp_driver_i2s
                                     esp_driver_spi esp_event esp_eth esp_http_server esp_netif
                                     nvs_flash esp_timer fatfs wear_levelling vfs lwip)
