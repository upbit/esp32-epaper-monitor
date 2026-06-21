#include "wifi_sta.hpp"
#include "config.h"

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char* TAG = "WIFI";

static EventGroupHandle_t s_eg = nullptr;
static const int BIT_CONNECTED = BIT0;
static volatile bool s_connected = false;
static char s_ip[16] = "0.0.0.0";

static void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA start, connecting to '%s'", WIFI_SSID);
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                strcpy(s_ip, "0.0.0.0");
                if (s_eg) xEventGroupClearBits(s_eg, BIT_CONNECTED);
                ESP_LOGW(TAG, "disconnected, reconnecting...");
                esp_wifi_connect();
                break;
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = (ip_event_got_ip_t*)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        if (s_eg) xEventGroupSetBits(s_eg, BIT_CONNECTED);
        ESP_LOGI(TAG, "got IP: %s", s_ip);
    }
}

void wifi_start() {
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, nullptr, nullptr));

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_wait_connected(int timeout_ms) {
    if (!s_eg) return false;
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

bool        wifi_is_connected() { return s_connected; }
const char* wifi_ip()           { return s_ip; }
const char* wifi_ssid()         { return WIFI_SSID; }
