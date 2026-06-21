// config.h - central, override-able configuration for e-paper-monitor.
//
// Every value can be overridden via -D build_flags in platformio.ini / secrets.ini.
// Business code must read only from here, so a config change needs only a rebuild.
#pragma once

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

// ---------------------------------------------------------------------------
// WiFi credentials (override in secrets.ini)
// ---------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "your-pass"
#endif

// ---------------------------------------------------------------------------
// Scrutiny API
//   Base URL WITHOUT trailing slash; business code appends "/summary" etc.
// ---------------------------------------------------------------------------
#ifndef SCRUTINY_API_BASE
#define SCRUTINY_API_BASE "http://192.168.66.2:8085/api"
#endif

// ---------------------------------------------------------------------------
// Cadence / behaviour tunables
// ---------------------------------------------------------------------------
#ifndef DISPLAY_PER_DISK_SECONDS
#define DISPLAY_PER_DISK_SECONDS 15
#endif
#ifndef FETCH_INTERVAL_SEC
#define FETCH_INTERVAL_SEC 600
#endif
#ifndef HTTP_FETCH_TIMEOUT_MS
#define HTTP_FETCH_TIMEOUT_MS 10000
#endif
#ifndef MAX_DISKS
#define MAX_DISKS 16
#endif
#ifndef PARTIAL_REFRESH_BUDGET
#define PARTIAL_REFRESH_BUDGET 20
#endif
#ifndef HTTP_SERVER_PORT
#define HTTP_SERVER_PORT 80
#endif

// Consecutive fetch failures before the UI shows a STALE marker.
#ifndef FETCH_FAIL_STALE_THRESHOLD
#define FETCH_FAIL_STALE_THRESHOLD 5
#endif

// ---------------------------------------------------------------------------
// E-paper wiring (verified hardware) - 2.13" V2 (GDEH0213B72 / SSD1675A)
// ---------------------------------------------------------------------------
#ifndef EPD_PIN_SCK
#define EPD_PIN_SCK 18
#endif
#ifndef EPD_PIN_MOSI
#define EPD_PIN_MOSI 23
#endif
#ifndef EPD_PIN_CS
#define EPD_PIN_CS 27
#endif
#ifndef EPD_PIN_DC
#define EPD_PIN_DC 26
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST 25
#endif
#ifndef EPD_PIN_BUSY
#define EPD_PIN_BUSY 33
#endif

// SPI clock for the panel (datasheet allows more; keep <= 4 MHz per spec).
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ (4 * 1000 * 1000)
#endif
