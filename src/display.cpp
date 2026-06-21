#include "display.hpp"
#include "epd_gfx.hpp"

#include <cstring>
#include <cstdio>
#include "esp_log.h"

static const char *TAG = "DISP";

// Layout enum used by the refresh policy.
enum Layout
{
    LAYOUT_INFO = 0,
    LAYOUT_DISK = 1,
    LAYOUT_MSG = 2
};

static Ssd1675a213 s_panel;
static EpdGfx s_gfx(s_panel);

static bool s_first = true;
static int s_partial = 0;
static int s_last_layout = -1;

// The active glass on the current 2.13" module is about 212x104 in the
// orientation used by this app. Keep the disk UI inside this safe area even
// though the controller framebuffer is larger.
static constexpr int DISK_UI_W = 212;
static constexpr int DISK_UI_H = 104;
static constexpr int DISK_UI_X = 0;
static constexpr int DISK_UI_Y = 18;

esp_err_t display_init()
{
    esp_err_t err = s_gfx.begin();
    if (err != ESP_OK)
        return err;
    s_gfx.setRotation(1); // controller landscape; disk UI uses 212x104 safe area
    return ESP_OK;
}

// Decide full vs partial refresh (requirement 5.2), then present + sleep.
static void present(Layout layout)
{
    const bool full = s_first || (int)layout != s_last_layout || s_partial >= PARTIAL_REFRESH_BUDGET;
    if (full)
    {
        ESP_LOGI(TAG, "full refresh (layout=%d)", (int)layout);
        s_gfx.displayFull();
        s_partial = 0;
    }
    else
    {
        s_gfx.displayPartial();
        ++s_partial;
    }
    s_first = false;
    s_last_layout = (int)layout;
    s_gfx.hibernate(); // low power between updates; partial re-uploads baseline
}

// --- helpers ---------------------------------------------------------------
static void draw_badge(EpdGfx &g, int x, int y, const char *label, bool filled, uint8_t size)
{
    int w = (int)strlen(label) * EpdGfx::charW(size) + 5;
    int h = EpdGfx::charH(size);
    if (filled)
    {
        g.fillRect(x, y, w, h, EPD_BLACK);
        g.setTextColor(EPD_WHITE, EPD_BLACK);
    }
    else
    {
        g.drawRect(x, y, w, h, EPD_BLACK);
        g.setTextColor(EPD_BLACK);
    }
    g.setTextSize(size);
    g.setCursor(x + 3, y + 1);
    g.print(label);
}

static const char *health_label(DiskHealth h)
{
    switch (h)
    {
    case DiskHealth::OK:
        return "OK";
    case DiskHealth::WARN:
        return "WARN";
    default:
        return "FAIL";
    }
}

static void draw_stale(EpdGfx &g)
{
    const int w = (g.width() < DISK_UI_W) ? g.width() : DISK_UI_W;
    draw_badge(g, DISK_UI_X + w - 4 - (5 * EpdGfx::charW(1) + 5),
               DISK_UI_Y + DISK_UI_H - 10, "STALE", true, 1);
}

static void draw_frame(EpdGfx &g)
{
    const int x = DISK_UI_X;
    const int y = DISK_UI_Y;
    const int w = DISK_UI_W;
    const int h = DISK_UI_H;
    g.drawHLine(x + 5, y, w - 10, EPD_BLACK);
    g.drawHLine(x + 5, y + h - 1, w - 10, EPD_BLACK);
    g.drawVLine(x, y + 5, h - 10, EPD_BLACK);
    g.drawVLine(x + w - 1, y + 5, h - 10, EPD_BLACK);
    g.drawLine(x, y + 5, x + 5, y, EPD_BLACK);
    g.drawLine(x + w - 6, y, x + w - 1, y + 5, EPD_BLACK);
    g.drawLine(x, y + h - 6, x + 5, y + h - 1, EPD_BLACK);
    g.drawLine(x + w - 6, y + h - 1, x + w - 1, y + h - 6, EPD_BLACK);
}

static void print_truncated(EpdGfx &g, const char *s, int max_px)
{
    if (!s)
        return;
    const int cw = EpdGfx::charW(1);
    char buf[64];
    int max_chars = max_px / cw;
    if (max_chars <= 0)
        return;
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int len = (int)strlen(buf);
    if (len > max_chars)
    {
        if (max_chars > 1)
        {
            buf[max_chars - 1] = '~';
            buf[max_chars] = '\0';
        }
        else
        {
            buf[0] = '\0';
        }
    }
    g.print(buf);
}

static void draw_storage_icon(EpdGfx &g, int x, int y, int w, int h, const char *type)
{
    g.drawRect(x, y, w, h, EPD_BLACK);
    g.drawRect(x + 2, y + 2, w - 4, h - 4, EPD_BLACK);

    if (type && strcmp(type, "NVMe") == 0)
    {
        // Simplified M.2 module.
        g.drawRect(x + 5, y + 11, w - 10, 12, EPD_BLACK);
        g.fillRect(x + 9, y + 14, 5, 5, EPD_BLACK);
        g.drawRect(x + 18, y + 14, 7, 5, EPD_BLACK);
        g.drawRect(x + 28, y + 14, 7, 5, EPD_BLACK);
        g.drawCircle(x + w - 9, y + 17, 2, EPD_BLACK);
        for (int i = 0; i < 5; ++i)
            g.drawVLine(x + 7 + i * 3, y + 25, 4, EPD_BLACK);
    }
    else
    {
        // Simplified 3.5" SATA/HDD face.
        int cx = x + w / 2;
        int cy = y + h / 2 + 1;
        g.drawCircle(cx, cy, 12, EPD_BLACK);
        g.drawCircle(cx, cy, 4, EPD_BLACK);
        g.drawLine(cx + 3, cy + 2, x + w - 8, y + h - 8, EPD_BLACK);
        g.drawCircle(x + w - 8, y + h - 8, 2, EPD_BLACK);
        g.drawCircle(x + 5, y + 5, 1, EPD_BLACK);
        g.drawCircle(x + w - 6, y + 5, 1, EPD_BLACK);
        g.drawCircle(x + 5, y + h - 6, 1, EPD_BLACK);
        g.drawCircle(x + w - 6, y + h - 6, 1, EPD_BLACK);
    }
}

static void draw_capacity_icon(EpdGfx &g, int x, int y)
{
    g.drawRect(x + 2, y + 4, 12, 12, EPD_BLACK);
    g.drawHLine(x + 3, y + 7, 10, EPD_BLACK);
    g.drawHLine(x + 3, y + 10, 10, EPD_BLACK);
    g.drawHLine(x + 3, y + 13, 10, EPD_BLACK);
}

static void draw_thermo_icon(EpdGfx &g, int x, int y)
{
    g.drawRect(x + 5, y + 1, 4, 12, EPD_BLACK);
    g.drawCircle(x + 7, y + 15, 4, EPD_BLACK);
    g.fillRect(x + 6, y + 8, 2, 6, EPD_BLACK);
}

static void draw_clock_icon(EpdGfx &g, int x, int y)
{
    g.drawCircle(x + 7, y + 7, 6, EPD_BLACK);
    g.drawLine(x + 7, y + 7, x + 7, y + 3, EPD_BLACK);
    g.drawLine(x + 7, y + 7, x + 11, y + 7, EPD_BLACK);
}

static void draw_metric_box(EpdGfx &g, int x, int y, int w, int h, const char *label)
{
    g.drawRect(x, y, w, h, EPD_BLACK);
    g.setTextColor(EPD_BLACK);
    g.setTextSize(1);
    g.setCursor(x + 20, y + 4);
    g.print(label);
}

static void draw_degree(EpdGfx &g, int x, int y)
{
    g.drawCircle(x, y, 1, EPD_BLACK);
}

static void draw_temp_value(EpdGfx &g, int x, int y, int32_t temp)
{
    g.setTextSize(2);
    g.setTextColor(EPD_BLACK);
    g.setCursor(x, y);
    g.printf("%d", (int)temp);
    int dx = g.cursorX() + 2;
    draw_degree(g, dx, y + 2);
    g.setTextSize(1);
    g.setCursor(dx + 4, y + 6);
    g.print("C");
}

static void print_capacity_value(EpdGfx &g, int x, int y, const char *label)
{
    char num[8] = "--";
    char unit[8] = "";
    if (label && label[0] && strcmp(label, "--") != 0)
    {
        sscanf(label, "%7s %7s", num, unit);
    }
    g.setTextColor(EPD_BLACK);
    g.setTextSize(2);
    g.setCursor(x, y);
    g.print(num);
    g.setTextSize(1);
    g.setCursor(g.cursorX() + 2, y + 8);
    g.print(unit);
}

static void format_power_on(int64_t hours, char *out, size_t cap)
{
    long days = (long)(hours / 24);
    if (days >= 365)
    {
        int y10 = (int)((hours * 10 + (365 * 24 / 2)) / (365 * 24));
        snprintf(out, cap, "%d.%dy %lldh", y10 / 10, y10 % 10, (long long)hours);
    }
    else
    {
        snprintf(out, cap, "%ldd %lldh", days, (long long)hours);
    }
}

// --- pages -----------------------------------------------------------------
void display_show_info(const char *ssid, const char *ip, const char *version,
                       int last_fetch_min, int disks_count, bool stale)
{
    EpdGfx &g = s_gfx;
    g.clear(EPD_WHITE);

    // Title.
    g.setTextColor(EPD_BLACK);
    g.setTextSize(2);
    const char *title = "E-Paper Mon";
    g.setCursor((g.width() - g.textWidth(title)) / 2, 0);
    g.print(title);
    g.drawHLine(0, 18, g.width(), EPD_BLACK);

    // Body.
    g.setTextSize(1);
    g.setTextColor(EPD_BLACK);
    int y = 26;
    g.setCursor(2, y);
    g.printf("SSID: %s", ssid ? ssid : "-");
    y += 12;
    g.setCursor(2, y);
    g.printf("IP  : %s", ip ? ip : "-");
    y += 12;
    g.setCursor(2, y);
    g.printf("Ver : %s  %s", version ? version : "-", __DATE__);
    y += 12;
    if (disks_count >= 0)
    {
        g.setCursor(2, y);
        g.printf("Disks: %d", disks_count);
        y += 12;
    }
    if (last_fetch_min >= 0)
    {
        g.setCursor(2, y);
        if (last_fetch_min < 60)
            g.printf("Last fetch: %dm ago", last_fetch_min);
        else
            g.printf("Last fetch: %dh ago", last_fetch_min / 60);
        y += 12;
    }
    if (stale)
        draw_stale(g);

    present(LAYOUT_INFO);
}

void display_show_disk(const DiskView &v)
{
    EpdGfx &g = s_gfx;
    const Disk &d = *v.disk;
    g.clear(EPD_WHITE);

    const int ox = DISK_UI_X;
    const int oy = DISK_UI_Y;

    draw_frame(g);

    draw_storage_icon(g, ox + 8, oy + 12, 38, 40, d.device_type);
    draw_badge(g, ox + 54, oy + 8, d.device_type[0] ? d.device_type : "DISK", false, 1);
    draw_badge(g, ox + 92, oy + 8, health_label(d.health), d.health != DiskHealth::OK, 1);

    g.setTextColor(EPD_BLACK);
    g.setTextSize((strlen(d.device_name) <= 4) ? 3 : 2);
    g.setCursor(ox + 54, oy + ((strlen(d.device_name) <= 4) ? 22 : 26));
    g.print(d.device_name[0] ? d.device_name : "disk");

    g.setTextSize(1);
    g.setCursor(ox + 54, oy + 45);
    g.print("MODEL");
    g.setCursor(ox + 54, oy + 54);
    print_truncated(g, d.model_name, 145);

    draw_metric_box(g, ox + 8, oy + 63, 95, 25, "CAP");
    draw_capacity_icon(g, ox + 13, oy + 69);
    print_capacity_value(g, ox + 32, oy + 72, d.capacity_label);

    draw_metric_box(g, ox + 109, oy + 63, 95, 25, "TEMP");
    draw_thermo_icon(g, ox + 114, oy + 69);
    draw_temp_value(g, ox + 136, oy + 72, d.temp);

    g.drawRect(ox + 8, oy + 90, 196, 12, EPD_BLACK);
    draw_clock_icon(g, ox + 11, oy + 89);
    char pbuf[28];
    format_power_on(d.power_on_hours, pbuf, sizeof(pbuf));
    g.setTextSize(1);
    g.setCursor(ox + 30, oy + 93);
    print_truncated(g, pbuf, 165);

    char ibuf[24];
    snprintf(ibuf, sizeof(ibuf), "%d/%d", v.index + 1, v.total);
    g.setCursor(ox + 184, oy + 8);
    g.print(ibuf);

    if (v.stale)
        draw_stale(g);

    present(LAYOUT_DISK);
}

void display_show_message(const char *title, const char *body)
{
    EpdGfx &g = s_gfx;
    g.clear(EPD_WHITE);
    g.setTextColor(EPD_BLACK);
    g.setTextSize(2);
    g.setCursor((g.width() - g.textWidth(title)) / 2, 8);
    g.print(title);
    g.drawHLine(0, 30, g.width(), EPD_BLACK);
    g.setTextSize(1);
    g.setCursor(6, 44);
    g.print(body);
    present(LAYOUT_MSG);
}

void display_hibernate() { s_gfx.hibernate(); }

int display_busy_fail() { return s_gfx.busyFailCount(); }
