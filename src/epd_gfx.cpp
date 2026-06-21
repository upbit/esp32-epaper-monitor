#include "epd_gfx.hpp"
#include "font5x7.h"

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "GFX";

esp_err_t EpdGfx::begin() {
    esp_err_t err = _panel.init();
    if (err != ESP_OK) return err;
    _buf = (uint8_t*)heap_caps_malloc(Ssd1675a213::BUF_SIZE, MALLOC_CAP_DMA);
    _old = (uint8_t*)heap_caps_malloc(Ssd1675a213::BUF_SIZE, MALLOC_CAP_DMA);
    if (!_buf || !_old) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    clear(EPD_WHITE);
    memset(_old, 0xFF, Ssd1675a213::BUF_SIZE); // old = white
    return ESP_OK;
}

void EpdGfx::setRotation(uint8_t r) { _rot = r & 3; }

void EpdGfx::clear(EpdColor c) {
    if (!_buf) return;
    memset(_buf, (c == EPD_WHITE) ? 0xFF : 0x00, Ssd1675a213::BUF_SIZE);
}

// Map a logical pixel (after rotation) to the physical RAM bit.
inline void EpdGfx::_setPhysical(int px, int py, EpdColor c) {
    if (px < 0 || px >= Ssd1675a213::WIDTH_VISIBLE || py < 0 || py >= Ssd1675a213::HEIGHT) return;
    const int idx = (px >> 3) + py * Ssd1675a213::ROW_BYTES;
    const uint8_t mask = 0x80 >> (px & 7);
    if (c == EPD_WHITE) _buf[idx] |= mask;   // 1 = white
    else                _buf[idx] &= ~mask;  // 0 = black (red -> black in mono)
}

void EpdGfx::drawPixel(int x, int y, EpdColor c) {
    if (!_buf) return;
    const int RW = Ssd1675a213::WIDTH_VISIBLE; // raw width 122
    const int RH = Ssd1675a213::HEIGHT;        // raw height 250
    int px, py;
    switch (_rot) {
        case 1: px = RW - 1 - y; py = x;            break; // landscape
        case 2: px = RW - 1 - x; py = RH - 1 - y;   break;
        case 3: px = y;          py = RH - 1 - x;   break;
        default: px = x;         py = y;            break;
    }
    _setPhysical(px, py, c);
}

void EpdGfx::drawHLine(int x, int y, int w, EpdColor c) {
    for (int i = 0; i < w; ++i) drawPixel(x + i, y, c);
}
void EpdGfx::drawVLine(int x, int y, int h, EpdColor c) {
    for (int i = 0; i < h; ++i) drawPixel(x, y + i, c);
}

void EpdGfx::drawLine(int x0, int y0, int x1, int y1, EpdColor c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        drawPixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void EpdGfx::drawRect(int x, int y, int w, int h, EpdColor c) {
    if (w <= 0 || h <= 0) return;
    drawHLine(x, y, w, c);
    drawHLine(x, y + h - 1, w, c);
    drawVLine(x, y, h, c);
    drawVLine(x + w - 1, y, h, c);
}

void EpdGfx::fillRect(int x, int y, int w, int h, EpdColor c) {
    for (int j = 0; j < h; ++j) drawHLine(x, y + j, w, c);
}

void EpdGfx::drawCircle(int cx, int cy, int r, EpdColor c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        drawPixel(cx + x, cy + y, c); drawPixel(cx + y, cy + x, c);
        drawPixel(cx - y, cy + x, c); drawPixel(cx - x, cy + y, c);
        drawPixel(cx - x, cy - y, c); drawPixel(cx - y, cy - x, c);
        drawPixel(cx + y, cy - x, c); drawPixel(cx + x, cy - y, c);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

void EpdGfx::drawChar(int x, int y, char c, EpdColor fg, EpdColor bg, bool bg_on, uint8_t size) {
    uint8_t idx = (c < 0x20 || c > 0x7E) ? 0 : (uint8_t)(c - 0x20);
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = FONT5X7[idx][col];
        for (int row = 0; row < 7; ++row) {
            bool on = bits & (1 << row);
            EpdColor pc = on ? fg : bg;
            if (!on && !bg_on) continue;
            if (size == 1) drawPixel(x + col, y + row, pc);
            else fillRect(x + col * size, y + row * size, size, size, pc);
        }
    }
    // 6th spacing column.
    if (bg_on) {
        if (size == 1) drawVLine(x + 5, y, 7, bg);
        else fillRect(x + 5 * size, y, size, 7 * size, bg);
    }
}

void EpdGfx::print(const char* s) {
    if (!s) return;
    for (; *s; ++s) {
        if (*s == '\n') { _cy += charH(_tsize); _cx = 0; continue; }
        drawChar(_cx, _cy, *s, _fg, _bg, _bg_on, _tsize);
        _cx += charW(_tsize);
    }
}

void EpdGfx::printf(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    print(buf);
}

int EpdGfx::textWidth(const char* s) const {
    if (!s) return 0;
    return (int)strlen(s) * charW(_tsize);
}

void EpdGfx::displayFull() {
    _panel.refreshFull(_buf);
    memcpy(_old, _buf, Ssd1675a213::BUF_SIZE);
}

void EpdGfx::displayPartial() {
    _panel.refreshPartial(_buf, _old);
    memcpy(_old, _buf, Ssd1675a213::BUF_SIZE);
}
