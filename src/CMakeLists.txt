# Main application component (PlatformIO honours this file under framework=espidf).
file(GLOB_RECURSE APP_SOURCES "*.cpp" "*.c")

idf_component_register(
    SRCS ${APP_SOURCES}
    INCLUDE_DIRS "." "../include"
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_http_server
        esp_timer
        json
        lwip
)
