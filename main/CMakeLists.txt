idf_component_register(SRCS "ir_test.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver)
