idf_component_register(SRCS "router.c" INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash json
    EMBED_TXTFILES "index.html")
