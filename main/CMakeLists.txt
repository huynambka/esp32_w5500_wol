idf_component_register(SRCS "main.c" "app_config.c" "w5500_net.c"
                            "provisioning.c" "blynk_client.c"
                       PRIV_REQUIRES esp_eth esp_event esp_netif esp_driver_spi esp_driver_gpio w5500
                                     esp_wifi esp_http_server esp-tls mbedtls
                                     nvs_flash esp_timer esp_hw_support lwip
                       INCLUDE_DIRS ".")
