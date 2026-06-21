// epd_gfx.hpp - lightweight Adafruit_GFX-style renderer over an EPD framebuffer.
//
// Mono framebuffer convention (matches the SSD1675A driver): bit 1 = WHITE.
// A second (red) plane is reserved for future 3-color panels; in mono mode
// EPD_RED is drawn as black.
#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"
#include "epd_ssd1675a.hpp"

enum EpdColor : uint8_t { EPD_WHITE = 0, EPD_BLACK = 1, EPD_RED = 2 };

class EpdGfx {
public:
    explicit EpdGfx(Ssd1675a213& panel) : _panel(panel) {}

    esp_err_t begin();                 // init panel + allocate buffers
    void setRotation(uint8_t r);       // 0..3 (we use 1 = landscape 250x122)
    int  width()  const { return (_rot & 1) ? Ssd1675a213::HEIGHT : Ssd1675a213::WIDTH_VISIBLE; }
    int  height() const { return (_rot & 1) ? Ssd1675a213::WIDTH_VISIBLE : Ssd1675a213::HEIGHT; }

    // Drawing primitives.
    void clear(EpdColor c = EPD_WHITE);
    void drawPixel(int x, int y, EpdColor c);
    void drawHLine(int x, int y, int w, EpdColor c);
    void drawVLine(int x, int y, int h, EpdColor c);
    void drawLine(int x0, int y0, int x1, int y1, EpdColor c);
    void drawRect(int x, int y, int w, int h, EpdColor c);
    void fillRect(int x, int y, int w, int h, EpdColor c);
    void drawCircle(int cx, int cy, int r, EpdColor c);

    // Text.
    void setCursor(int x, int y) { _cx = x; _cy = y; }
    void setTextColor(EpdColor fg) { _fg = fg; _bg_on = false; }
    void setTextColor(EpdColor fg, EpdColor bg) { _fg = fg; _bg = bg; _bg_on = true; }
    void setTextSize(uint8_t s) { _tsize = s ? s : 1; }
    int  cursorX() const { return _cx; }
    int  cursorY() const { return _cy; }
    void drawChar(int x, int y, char c, EpdColor fg, EpdColor bg, bool bg_on, uint8_t size);
    void print(const char* s);
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    // Pixel width of `s` at the current text size (6*size per char).
    int  textWidth(const char* s) const;
    static constexpr int charW(uint8_t size) { return 6 * size; }
    static constexpr int charH(uint8_t size) { return 8 * size; }

    // Present the framebuffer to the panel.
    void displayFull();
    void displayPartial();
    void hibernate() { _panel.hibernate(); }
    int  busyFailCount() const { return _panel.busyFailCount(); }

private:
    Ssd1675a213& _panel;
    uint8_t* _buf = nullptr;   // current frame (DMA capable)
    uint8_t* _old = nullptr;   // last shown frame (partial baseline)
    uint8_t  _rot = 1;
    int      _cx = 0, _cy = 0;
    EpdColor _fg = EPD_BLACK, _bg = EPD_WHITE;
    bool     _bg_on = false;
    uint8_t  _tsize = 1;

    inline void _setPhysical(int px, int py, EpdColor c);
};
