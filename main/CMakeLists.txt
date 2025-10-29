idf_component_register(
  SRCS "hello_world_main.c"
  INCLUDE_DIRS "."

  PRIV_REQUIRES
    spi_flash
    driver
    esp_timer
    freertos
    log
    esp_common
    esp_hw_support
    nvs_flash
    esp_wifi
    esp_netif
    lwip
    esp_event
)
# Se o warning do driver legacy virar erro:
# target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)