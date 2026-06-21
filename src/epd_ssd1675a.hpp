// epd_ssd1675a.hpp - thin ESP-IDF driver for SSD1675A / IL3897 (GDEH0213B72, 2.13" V2).
//
// Init sequence, LUTs and refresh flow are ported verbatim from
// GxEPD2 (ZinggJM/GxEPD2, file src/epd/GxEPD2_213_B72.cpp, GPL-3.0).
// This keeps refresh behaviour identical to the already-validated hardware test.
//
// Buffer convention (matches GxEPD2_BW): bit = 1 -> WHITE, bit = 0 -> BLACK.
// The buffer is sent directly to RAM command 0x24 (no inversion).
//
// The SSD167x/SSD168x command set is shared, so future 3-color panels
// (SSD1680/SSD1683) can be added as sibling classes with their own
// resolution/LUT and an extra red RAM plane (command 0x26).
#pragma once

#include <cstdint>
#include <cstddef>
#include "driver/spi_master.h"
#include "config.h"

class Ssd1675a213 {
public:
    // Physical panel geometry.
    static constexpr int      RAM_WIDTH    = 128;          // controller RAM width (bits)
    static constexpr int      WIDTH_VISIBLE = 122;         // visible width (pixels)
    static constexpr int      HEIGHT       = 250;          // pixels
    static constexpr int      ROW_BYTES    = RAM_WIDTH / 8; // 16
    static constexpr size_t   BUF_SIZE     = (size_t)ROW_BYTES * HEIGHT; // 4000 bytes

    // Set up GPIO + SPI bus/device. Returns ESP_OK on success.
    esp_err_t init();

    // Full refresh: slowest, removes all ghosting. `bw` is BUF_SIZE bytes.
    void refreshFull(const uint8_t* bw);

    // Fast differential refresh using the part LUT. `bw` = new frame,
    // `bw_old` = previously displayed frame (baseline for the transition).
    void refreshPartial(const uint8_t* bw, const uint8_t* bw_old);

    // Power off driver stage and enter deep sleep (lowest power).
    void hibernate();

    // Counts consecutive BUSY timeouts; reset on success.
    int  busyFailCount() const { return _busy_fail; }

private:
    spi_device_handle_t _spi = nullptr;
    bool _hibernating = true;
    bool _power_on    = false;
    int  _busy_fail   = 0;

    // Low-level transfer helpers.
    void _cmd(uint8_t c);
    void _data(uint8_t d);
    void _dataBuf(const uint8_t* buf, size_t len);

    void _reset();
    bool _waitBusy(const char* what, int timeout_ms);

    void _initDisplay();
    void _initFull();
    void _initPart();
    void _powerOn();
    void _powerOff();
    void _updateFull();
    void _updatePart();

    void _setRamAreaFull();                       // address window = whole panel
    void _writeRam(uint8_t cmd, const uint8_t* b); // 0x24 (bw) or 0x26 (old)
};
