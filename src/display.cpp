#include "display.hpp"
#include "epd_gfx.hpp"
#include "icons/nvme_bitmap.h"
#include "icons/sata_bitmap.h"
#include "icons/cap_bitmap.h"
#include "icons/temp_bitmap.h"

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

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool contains_ci(const char *s, const char *needle)
{
    if (!s || !needle || !*needle)
        return false;
    for (; *s; ++s)
    {
        const char *a = s;
        const char *b = needle;
        while (*a && *b && lower_ascii(*a) == lower_ascii(*b))
        {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
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

static void draw_mono_bitmap_scaled(EpdGfx &g, int x, int y, int w, int h,
                                    const uint8_t *bitmap, int src_w, int src_h, int row_bytes)
{
    if (!bitmap || w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0 || row_bytes <= 0)
        return;
    for (int dy = 0; dy < h; ++dy)
    {
        int sy = dy * src_h / h;
        for (int dx = 0; dx < w; ++dx)
        {
            int sx = dx * src_w / w;
            uint8_t b = bitmap[sy * row_bytes + sx / 8];
            if (b & (0x80 >> (sx & 7)))
                g.drawPixel(x + dx, y + dy, EPD_BLACK);
        }
    }
}

struct BitmapIcon
{
    const uint8_t *bitmap;
    int width;
    int height;
    int row_bytes;
};

static BitmapIcon storage_icon_for_disk(const Disk &d)
{
    if (contains_ci(d.device_type, "nvme") ||
        contains_ci(d.device_name, "nvme") ||
        contains_ci(d.model_name, "nvme"))
    {
        return {nvme_bitmap, nvme_width, nvme_height, nvme_row_bytes};
    }
    return {sata_bitmap, sata_width, sata_height, sata_row_bytes};
}

static void draw_storage_logo(EpdGfx &g, int x, int y, int w, int h, const Disk &d)
{
    const BitmapIcon icon = storage_icon_for_disk(d);
    int bw = w;
    int bh = h;
    if (icon.width * h > icon.height * w)
        bh = w * icon.height / icon.width;
    else
        bw = h * icon.width / icon.height;

    draw_mono_bitmap_scaled(g, x + (w - bw) / 2, y + (h - bh) / 2,
                            bw, bh, icon.bitmap, icon.width, icon.height, icon.row_bytes);
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
    // Label hugs the top-right corner: 2px top margin, 2px right margin. Glyph ink
    // spans (textWidth-1) px, so the ink's right edge lands at (x+w-2) when the
    // cursor is at x + w - 1 - textWidth.
    g.setCursor(x + w - 1 - g.textWidth(label), y + 2);
    g.print(label);
}

// Draw a 24x24 mono icon hugging the left edge of a metric box, vertically
// centered. Returns the x just right of the icon so callers can shift values.
static int draw_metric_icon(EpdGfx &g, int box_x, int box_y, int box_h,
                            const BitmapIcon &icon)
{
    const int pad = 4;
    int iy = box_y + (box_h - icon.height) / 2;
    draw_mono_bitmap_scaled(g, box_x + pad, iy, icon.width, icon.height,
                            icon.bitmap, icon.width, icon.height, icon.row_bytes);
    return box_x + pad + icon.width;
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

static int temp_value_width(int32_t temp)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (int)temp);
    return (int)strlen(buf) * EpdGfx::charW(2) + 2 + 3 + EpdGfx::charW(1);
}

static void draw_temp_value_right(EpdGfx &g, int right_x, int y, int32_t temp)
{
    draw_temp_value(g, right_x - temp_value_width(temp), y, temp);
}

static int capacity_value_width(const char *label)
{
    char num[8] = "--";
    char unit[8] = "";
    if (label && label[0] && strcmp(label, "--") != 0)
    {
        sscanf(label, "%7s %7s", num, unit);
    }
    return (int)strlen(num) * EpdGfx::charW(2) + 2 + (int)strlen(unit) * EpdGfx::charW(1);
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

static void print_capacity_value_right(EpdGfx &g, int right_x, int y, const char *label)
{
    print_capacity_value(g, right_x - capacity_value_width(label), y, label);
}

static void format_power_on(int64_t hours, char *out, size_t cap)
{
    long days = (long)(hours / 24);
    if (days >= 365)
    {
        int y10 = (int)((hours * 10 + (365 * 24 / 2)) / (365 * 24));
        snprintf(out, cap, "%d.%dy (%lldh)", y10 / 10, y10 % 10, (long long)hours);
    }
    else
    {
        snprintf(out, cap, "%ldd (%lldh)", days, (long long)hours);
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

    draw_storage_logo(g, ox + 8, oy + 6, 42, 42, d);

    const char *status = health_label(d.health);
    char ibuf[16];
    snprintf(ibuf, sizeof(ibuf), "%d/%d", v.index + 1, v.total);
    g.setTextColor(EPD_BLACK);
    g.setTextSize(1);

    // Right-aligned: [STATUS box]  index/total
    int idx_w = g.textWidth(ibuf);
    int idx_x = ox + DISK_UI_W - 8 - idx_w;
    g.setCursor(idx_x, oy + 7);
    g.print(ibuf);

    // Status badge with a 2px-padded outline to set it apart from the index text.
    // textWidth() = len*6, where each glyph is 5px ink + 1px trailing gap. So the
    // ink spans (textWidth-1) px; box_w = textWidth+3 gives 2px padding on left/right.
    int box_w = g.textWidth(status) + 3;
    int box_h = 11; // 8px glyph + 1px bottom + 2px top padding
    int box_x = idx_x - 6 - box_w;
    int box_y = oy + 5; // index text sits at oy+7, so box top is 2px above its glyphs
    g.drawRect(box_x, box_y, box_w, box_h, EPD_BLACK);
    g.setCursor(box_x + 2, box_y + 2);
    g.print(status);

    g.setTextColor(EPD_BLACK);
    g.setTextSize((strlen(d.device_name) <= 4) ? 3 : 2);
    g.setCursor(ox + 56, oy + ((strlen(d.device_name) <= 4) ? 9 : 13));
    g.print(d.device_name[0] ? d.device_name : "disk");

    g.setTextSize(1);
    g.setCursor(ox + 56, oy + 36);
    print_truncated(g, d.model_name, 145);

    // Metric value blocks are ~16px tall; box is 31px, so y = top + (31-16)/2
    // keeps the value vertically centered in the box.
    // CAP/TEMP widths follow the golden ratio (~0.618 / 0.382) across the 190px
    // span (with a 6px gap): CAP gets 117px (capacity strings are longer), TEMP
    // only 73px (temperatures are 2 digits). Outer extents are unchanged.
    draw_metric_box(g, ox + 8, oy + 50, 117, 31, "CAP");
    draw_metric_icon(g, ox + 8, oy + 50, 31,
                     {cap_bitmap, cap_width, cap_height, cap_row_bytes});
    print_capacity_value_right(g, ox + 118, oy + 62, d.capacity_label);

    draw_metric_box(g, ox + 131, oy + 50, 73, 31, "TEMP");
    draw_metric_icon(g, ox + 131, oy + 50, 31,
                     {temp_bitmap, temp_width, temp_height, temp_row_bytes});
    draw_temp_value_right(g, ox + 197, oy + 62, d.temp);

    draw_clock_icon(g, ox + 11, oy + 84);
    char pbuf[28];
    format_power_on(d.power_on_hours, pbuf, sizeof(pbuf));
    g.setTextSize(1);
    g.setCursor(ox + 30, oy + 88);
    print_truncated(g, pbuf, 165);

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
