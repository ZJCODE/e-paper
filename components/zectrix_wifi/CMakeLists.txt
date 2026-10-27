idf_component_register(
    SRCS "zectrix_wifi.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
