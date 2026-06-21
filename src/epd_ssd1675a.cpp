#include "epd_ssd1675a.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "EPD";

// --- LUTs ported verbatim from GxEPD2_213_B72.cpp (GPL-3.0) ---------------
static const uint8_t LUT_FULL[] = {
    0x80,
    0x60,
    0x40,
    0x00,
    0x00,
    0x00,
    0x00, // LUT0 BB
    0x10,
    0x60,
    0x20,
    0x00,
    0x00,
    0x00,
    0x00, // LUT1 BW
    0x80,
    0x60,
    0x40,
    0x00,
    0x00,
    0x00,
    0x00, // LUT2 WB
    0x10,
    0x60,
    0x20,
    0x00,
    0x00,
    0x00,
    0x00, // LUT3 WW
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT4 VCOM
    0x03,
    0x03,
    0x00,
    0x00,
    0x02, // TP0
    0x09,
    0x09,
    0x00,
    0x00,
    0x02, // TP1
    0x03,
    0x03,
    0x00,
    0x00,
    0x02, // TP2
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP3
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP4
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP5
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP6
};

static const uint8_t LUT_PART[] = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT0 BB
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT1 BW
    0x40,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT2 WB
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT3 WW
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // LUT4 VCOM
    0x0A,
    0x00,
    0x00,
    0x00,
    0x00, // TP0
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP1
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP2
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP3
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP4
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP5
    0x00,
    0x00,
    0x00,
    0x00,
    0x00, // TP6
};

// Timing constants from GxEPD2_213_B72.h (ms).
static constexpr int POWER_ON_MS = 100;
static constexpr int POWER_OFF_MS = 180;
static constexpr int FULL_MS = 1700;
static constexpr int PART_MS = 200;
static constexpr int BUSY_TIMEOUT_MS = 30000; // requirement 2.5

// SPI pre-transaction callback: drive DC from transaction->user (0=cmd,1=data).
static void IRAM_ATTR epd_spi_pre(spi_transaction_t *t)
{
    gpio_set_level((gpio_num_t)EPD_PIN_DC, (int)(intptr_t)t->user);
}

esp_err_t Ssd1675a213::init()
{
    // Control GPIOs.
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST);
    gpio_config(&out);

    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << EPD_PIN_BUSY);
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&in);

    gpio_set_level((gpio_num_t)EPD_PIN_RST, 1);
    gpio_set_level((gpio_num_t)EPD_PIN_DC, 0);

    // SPI bus.
    spi_bus_config_t bus = {};
    bus.mosi_io_num = EPD_PIN_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = EPD_PIN_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = EPD_SPI_HZ;
    dev.mode = 0;
    dev.spics_io_num = EPD_PIN_CS;
    dev.queue_size = 4;
    dev.pre_cb = epd_spi_pre;
    err = spi_bus_add_device(SPI3_HOST, &dev, &_spi);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    _hibernating = true; // force a hardware reset on first _initDisplay()
    ESP_LOGI(TAG, "driver ready (SSD1675A %dx%d, SPI %d Hz)", WIDTH_VISIBLE, HEIGHT, EPD_SPI_HZ);
    return ESP_OK;
}

// --- low level -------------------------------------------------------------
void Ssd1675a213::_cmd(uint8_t c)
{
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &c;
    t.user = (void *)0;
    spi_device_polling_transmit(_spi, &t);
}

void Ssd1675a213::_data(uint8_t d)
{
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &d;
    t.user = (void *)1;
    spi_device_polling_transmit(_spi, &t);
}

void Ssd1675a213::_dataBuf(const uint8_t *buf, size_t len)
{
    const size_t CHUNK = 2048;
    size_t off = 0;
    while (off < len)
    {
        size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = buf + off;
        t.user = (void *)1;
        spi_device_polling_transmit(_spi, &t);
        off += n;
    }
}

void Ssd1675a213::_reset()
{
    gpio_set_level((gpio_num_t)EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)EPD_PIN_RST, 0); // active-low assert
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    _hibernating = false;
    _power_on = false;
    _waitBusy("_reset", BUSY_TIMEOUT_MS);
}

bool Ssd1675a213::_waitBusy(const char *what, int timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(1)); // margin to let BUSY become active
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    // BUSY is active HIGH on this panel.
    while (gpio_get_level((gpio_num_t)EPD_PIN_BUSY) == 1)
    {
        if (xTaskGetTickCount() - start > limit)
        {
            ++_busy_fail;
            ESP_LOGE(TAG, "BUSY timeout in %s (>%d ms), fail#%d", what, timeout_ms, _busy_fail);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    _busy_fail = 0;
    return true;
}

// --- init / power ----------------------------------------------------------
void Ssd1675a213::_initDisplay()
{
    if (_hibernating)
        _reset();
    _cmd(0x74);
    _data(0x54); // set analog block control
    _cmd(0x7E);
    _data(0x3B); // set digital block control
    _cmd(0x01);
    _data(0xF9);
    _data(0x00);
    _data(0x00); // driver output control (HEIGHT-1=249)
    _cmd(0x3C);
    _data(0x03); // border waveform
    _cmd(0x2C);
    _data(0x70); // VCOM
    _cmd(0x03);
    _data(0x15); // gate driving voltage (19V)
    _cmd(0x04);
    _data(0x41);
    _data(0xA8);
    _data(0x32); // source driving voltage
    _cmd(0x3A);
    _data(0x30); // dummy line
    _cmd(0x3B);
    _data(0x0A); // gate time
    _setRamAreaFull();
}

void Ssd1675a213::_setRamAreaFull()
{
    _cmd(0x11);
    _data(0x03); // data entry: X inc, Y inc
    _cmd(0x44);
    _data(0x00);
    _data((RAM_WIDTH - 1) / 8); // RAM-X start/end (0..15)
    _cmd(0x45);                 // RAM-Y start/end (0..249)
    _data(0x00);
    _data(0x00);
    _data((HEIGHT - 1) % 256);
    _data((HEIGHT - 1) / 256);
    _cmd(0x4E);
    _data(0x00); // RAM-X counter
    _cmd(0x4F);
    _data(0x00);
    _data(0x00); // RAM-Y counter
}

void Ssd1675a213::_initFull()
{
    _initDisplay();
    _cmd(0x32);
    _dataBuf(LUT_FULL, sizeof(LUT_FULL));
    _powerOn();
}

void Ssd1675a213::_initPart()
{
    _initDisplay();
    _cmd(0x2C);
    _data(0x26); // VCOM override for partial
    _cmd(0x32);
    _dataBuf(LUT_PART, sizeof(LUT_PART));
    _powerOn();
}

void Ssd1675a213::_powerOn()
{
    if (!_power_on)
    {
        _cmd(0x22);
        _data(0xC0);
        _cmd(0x20);
        _waitBusy("_powerOn", POWER_ON_MS + 5000);
        _power_on = true;
    }
}

void Ssd1675a213::_powerOff()
{
    if (_power_on)
    {
        _cmd(0x22);
        _data(0xC3);
        _cmd(0x20);
        _waitBusy("_powerOff", POWER_OFF_MS + 5000);
        _power_on = false;
    }
}

void Ssd1675a213::_updateFull()
{
    _cmd(0x22);
    _data(0xC4);
    _cmd(0x20);
    _waitBusy("_updateFull", FULL_MS + 5000);
}

void Ssd1675a213::_updatePart()
{
    _cmd(0x22);
    _data(0x04);
    _cmd(0x20);
    _waitBusy("_updatePart", PART_MS + 5000);
}

void Ssd1675a213::_writeRam(uint8_t cmd, const uint8_t *b)
{
    _setRamAreaFull();
    _cmd(cmd);
    _dataBuf(b, BUF_SIZE);
}

// --- public refresh --------------------------------------------------------
void Ssd1675a213::refreshFull(const uint8_t *bw)
{
    _initFull();
    _writeRam(0x24, bw); // new frame
    _writeRam(0x26, bw); // baseline old = new (for subsequent partial)
    _updateFull();
    ESP_LOGI(TAG, "full refresh done");
}

void Ssd1675a213::refreshPartial(const uint8_t *bw, const uint8_t *bw_old)
{
    _initPart();
    _writeRam(0x26, bw_old); // previous frame
    _writeRam(0x24, bw);     // new frame
    _updatePart();
    ESP_LOGI(TAG, "partial refresh done");
}

void Ssd1675a213::hibernate()
{
    _powerOff();
    _cmd(0x10);
    _data(0x01); // deep sleep mode
    _hibernating = true;
    ESP_LOGI(TAG, "hibernated (deep sleep)");
}
