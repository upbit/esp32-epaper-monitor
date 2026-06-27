// config.h - central, override-able configuration for e-paper-monitor.
//
// Every value can be overridden via -D build_flags in platformio.ini / secrets.ini.
// Business code must read only from here, so a config change needs only a rebuild.
#pragma once

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
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
#define DISPLAY_PER_DISK_SECONDS 30
#endif
#ifndef FETCH_INTERVAL_SEC
#define FETCH_INTERVAL_SEC 3600
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
// E-paper wiring - 2.13" V2 (GDEH0213B72 / SSD1675A)
//
// Defaults below target ESP32-S3-DevKitC-1 and map onto its FSPI IOMUX pins
// (FSPICLK=12 / FSPID=11 / FSPICS0=10), so the SPI bus runs through IOMUX.
// Every pin can be overridden via -D build_flags (see platformio.ini), e.g.
// to move back to an ESP32-WROOM-32:
//   -DEPD_PIN_SCK=18 -DEPD_PIN_SDA=23 -DEPD_PIN_CS=27
//   -DEPD_PIN_DC=26  -DEPD_PIN_RST=25 -DEPD_PIN_BUSY=33 -DEPD_SPI_HOST=SPI3_HOST
//
// Pin macros follow the e-paper driver-board labels (SCK/SDA/RST/DC/CS/BUSY);
// EPD_PIN_SDA is the panel's serial-data line (SPI MOSI), not an I2C SDA.
// ---------------------------------------------------------------------------
#ifndef EPD_PIN_SCK
#define EPD_PIN_SCK 12
#endif
#ifndef EPD_PIN_SDA
#define EPD_PIN_SDA 11
#endif
#ifndef EPD_PIN_CS
#define EPD_PIN_CS 10
#endif
#ifndef EPD_PIN_DC
#define EPD_PIN_DC 9
#endif
#ifndef EPD_PIN_RST
#define EPD_PIN_RST 14
#endif
#ifndef EPD_PIN_BUSY
#define EPD_PIN_BUSY 3
#endif

// SPI peripheral used to drive the panel. On ESP32-S3 SPI2_HOST (FSPI) owns the
// IOMUX pins used above; on the classic ESP32 use SPI3_HOST (VSPI).
#ifndef EPD_SPI_HOST
#define EPD_SPI_HOST SPI2_HOST
#endif

// SPI clock for the panel (datasheet allows more; keep <= 4 MHz per spec).
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ (4 * 1000 * 1000)
#endif
