// wifi_sta.hpp - STA-mode WiFi with auto-reconnect (ESP-IDF esp_wifi).
#pragma once

#include <cstdbool>

// Initialise netif/event/wifi and start connecting using compile-time creds.
void wifi_start();

// Block until connected (got IP) or timeout. Returns true if connected.
bool wifi_wait_connected(int timeout_ms);

bool wifi_is_connected();
const char *wifi_ip();   // "x.x.x.x" or "0.0.0.0"
const char *wifi_ssid(); // configured SSID
