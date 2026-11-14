idf_component_register(SRCS "main.c" "printer_monitor.c" INCLUDE_DIRS "." EMBED_TXTFILES "bambu_ca_bundle.pem"
    REQUIRES espressif__esp_codec_dev lvgl__lvgl esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_hw_support lwip json esp_timer mqtt)
