// E-Paper Monitor - 2.13" V2 (GxEPD2_213_B72 / GDEH0213B72, SSD1675A) refresh test
//
// Migrated from the ink_test example (GxEPD2 by Jean-Marc Zingg).
// Library: https://github.com/ZinggJM/GxEPD2
//
// ===================== Wiring (ESP32 DevKit) =====================
//   E-Paper Driver Board        ESP32 DevKit
//   GND      -------->  GND
//   3V3      -------->  3V3
//   SCK      -------->  GPIO18  (HSPI/VSPI SCK)
//   SDA(DIN) -------->  GPIO23  (MOSI)
//   RST      -------->  GPIO25
//   DC       -------->  GPIO26
//   CS1      -------->  GPIO27
//   BUSY     -------->  GPIO33
//   CS2      -------->  (not connected)
// =================================================================

#include <Arduino.h>
#include <SPI.h>

// Enable GxEPD2_GFX base class (optional but matches the example style)
#define ENABLE_GxEPD2_GFX 1

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>

// ---- Pin mapping (matches the wiring table above) ----
#define EPD_CS 27
#define EPD_DC 26
#define EPD_RST 25
#define EPD_BUSY 33
// SCK = 18, MOSI = 23 are ESP32 hardware-SPI defaults; no remap needed.

// ---- Display instance: 2.13" V2 (GDEH0213B72) ----
GxEPD2_BW<GxEPD2_213_B72, GxEPD2_213_B72::HEIGHT> display(
    GxEPD2_213_B72(/*CS=*/EPD_CS, /*DC=*/EPD_DC, /*RST=*/EPD_RST, /*BUSY=*/EPD_BUSY));

// ---------------------------------------------------------------------------
// Test routines
// ---------------------------------------------------------------------------

static void helloWorld()
{
    display.setRotation(1);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    const char text[] = "Hello World!";
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(text, 0, 0, &tbx, &tby, &tbw, &tbh);
    uint16_t x = ((display.width() - tbw) / 2) - tbx;
    uint16_t y = ((display.height() - tbh) / 2) - tby;

    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(x, y);
        display.print(text);
    } while (display.nextPage());
}

static void drawBorderAndCorners()
{
    display.setRotation(1);
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        // outer border
        display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
        // corner markers (helps to verify orientation & full coverage)
        display.fillRect(0, 0, 10, 10, GxEPD_BLACK);
        display.fillRect(display.width() - 10, 0, 10, 10, GxEPD_BLACK);
        display.fillRect(0, display.height() - 10, 10, 10, GxEPD_BLACK);
        display.fillRect(display.width() - 10, display.height() - 10, 10, 10, GxEPD_BLACK);
        // diagonal cross to confirm pixels everywhere
        display.drawLine(0, 0, display.width() - 1, display.height() - 1, GxEPD_BLACK);
        display.drawLine(display.width() - 1, 0, 0, display.height() - 1, GxEPD_BLACK);
    } while (display.nextPage());
}

static void partialUpdateCounter()
{
    if (!display.epd2.hasPartialUpdate)
    {
        Serial.println("[INFO] Panel has no partial update support, skip.");
        return;
    }

    display.setRotation(1);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);

    const char *label = "Count:";
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(label, 0, 0, &tbx, &tby, &tbw, &tbh);

    uint16_t lx = 10 - tbx;
    uint16_t ly = ((display.height() - tbh) / 2) - tby;

    // Region for the counter number, to the right of the label.
    uint16_t nx = lx + tbw + 8;
    uint16_t ny = ly;
    uint16_t nw = display.width() - nx - 10;
    uint16_t nh = tbh + 6;

    // Full refresh first to draw the static label.
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(lx, ly);
        display.print(label);
    } while (display.nextPage());

    // Then partial refresh just for the changing counter region.
    for (int i = 0; i < 5; ++i)
    {
        display.setPartialWindow(nx, ny - tbh, nw, nh);
        display.firstPage();
        do
        {
            display.fillScreen(GxEPD_WHITE);
            display.setCursor(nx, ny);
            display.print(i);
        } while (display.nextPage());
        delay(1000);
    }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("E-Paper 2.13\" V2 (GxEPD2_213_B72) refresh test starting...");

    // Re-route HW SPI to the wired pins (SCK=18, MISO=19 unused, MOSI=23, SS=CS).
    // Most ESP32 Arduino cores already default to these pins, but doing it
    // explicitly makes the wiring above match the code.
    SPI.begin(/*SCK=*/18, /*MISO=*/19, /*MOSI=*/23, /*SS=*/EPD_CS);

    // 20ms reset pulse — default for bare panels with DESPI-C02 / direct wiring.
    display.init(115200, true, 20, false);

    Serial.println("[1/3] Full refresh: Hello World");
    helloWorld();
    delay(2000);

    Serial.println("[2/3] Full refresh: border + corners + cross");
    drawBorderAndCorners();
    delay(2000);

    Serial.println("[3/3] Partial refresh: counter");
    partialUpdateCounter();

    display.hibernate();
    Serial.println("Done. Panel hibernated.");
}

void loop()
{
    // Nothing to do; the test runs once in setup().
}
