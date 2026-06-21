#include "display.hpp"
#include "epd_gfx.hpp"

#include <cstring>
#include <cstdio>
#include "esp_log.h"

static const char* TAG = "DISP";

// Layout enum used by the refresh policy.
enum Layout { LAYOUT_INFO = 0, LAYOUT_DISK = 1, LAYOUT_MSG = 2 };

static Ssd1675a213 s_panel;
static EpdGfx       s_gfx(s_panel);

static bool s_first   = true;
static int  s_partial = 0;
static int  s_last_layout = -1;

esp_err_t display_init() {
    esp_err_t err = s_gfx.begin();
    if (err != ESP_OK) return err;
    s_gfx.setRotation(1); // landscape 250x122
    return ESP_OK;
}

// Decide full vs partial refresh (requirement 5.2), then present + sleep.
static void present(Layout layout) {
    const bool full = s_first
                   || (int)layout != s_last_layout
                   || s_partial >= PARTIAL_REFRESH_BUDGET;
    if (full) {
        ESP_LOGI(TAG, "full refresh (layout=%d)", (int)layout);
        s_gfx.displayFull();
        s_partial = 0;
    } else {
        s_gfx.displayPartial();
        ++s_partial;
    }
    s_first = false;
    s_last_layout = (int)layout;
    s_gfx.hibernate(); // low power between updates; partial re-uploads baseline
}

// --- helpers ---------------------------------------------------------------
static void draw_badge(EpdGfx& g, int x, int y, const char* label, bool filled, uint8_t size) {
    int w = (int)strlen(label) * EpdGfx::charW(size) + 5;
    int h = EpdGfx::charH(size);
    if (filled) {
        g.fillRect(x, y, w, h, EPD_BLACK);
        g.setTextColor(EPD_WHITE, EPD_BLACK);
    } else {
        g.drawRect(x, y, w, h, EPD_BLACK);
        g.setTextColor(EPD_BLACK);
    }
    g.setTextSize(size);
    g.setCursor(x + 3, y + 1);
    g.print(label);
}

static const char* health_label(DiskHealth h) {
    switch (h) {
        case DiskHealth::OK:   return "OK";
        case DiskHealth::WARN: return "WARN";
        default:               return "FAIL";
    }
}

static void draw_stale(EpdGfx& g) {
    draw_badge(g, g.width() - 4 - (4 * EpdGfx::charW(1) + 5), g.height() - 10, "STALE", true, 1);
}

// --- pages -----------------------------------------------------------------
void display_show_info(const char* ssid, const char* ip, const char* version,
                       int last_fetch_min, int disks_count, bool stale) {
    EpdGfx& g = s_gfx;
    g.clear(EPD_WHITE);

    // Title.
    g.setTextColor(EPD_BLACK);
    g.setTextSize(2);
    const char* title = "E-Paper Mon";
    g.setCursor((g.width() - g.textWidth(title)) / 2, 0);
    g.print(title);
    g.drawHLine(0, 18, g.width(), EPD_BLACK);

    // Body.
    g.setTextSize(1);
    g.setTextColor(EPD_BLACK);
    int y = 26;
    g.setCursor(2, y); g.printf("SSID: %s", ssid ? ssid : "-");          y += 12;
    g.setCursor(2, y); g.printf("IP  : %s", ip ? ip : "-");              y += 12;
    g.setCursor(2, y); g.printf("Ver : %s  %s", version ? version : "-", __DATE__); y += 12;
    if (disks_count >= 0) { g.setCursor(2, y); g.printf("Disks: %d", disks_count); y += 12; }
    if (last_fetch_min >= 0) {
        g.setCursor(2, y);
        if (last_fetch_min < 60) g.printf("Last fetch: %dm ago", last_fetch_min);
        else                     g.printf("Last fetch: %dh ago", last_fetch_min / 60);
        y += 12;
    }
    if (stale) draw_stale(g);

    present(LAYOUT_INFO);
}

void display_show_disk(const DiskView& v) {
    EpdGfx& g = s_gfx;
    const Disk& d = *v.disk;
    g.clear(EPD_WHITE);

    // --- top status bar ---
    g.setTextSize(1);
    g.setTextColor(EPD_BLACK);
    g.setCursor(2, 2);
    g.printf("%d/%d", v.index + 1, v.total);

    // health badge (centered-ish)
    const char* hl = health_label(d.health);
    bool filled = (d.health != DiskHealth::OK);
    int badge_w = (int)strlen(hl) * EpdGfx::charW(1) + 5;
    draw_badge(g, (g.width() - badge_w) / 2, 1, hl, filled, 1);

    // time "Xm ago" right aligned
    char tbuf[24];
    if (v.minutes_since_fetch < 60) snprintf(tbuf, sizeof(tbuf), "%dm ago", v.minutes_since_fetch);
    else                            snprintf(tbuf, sizeof(tbuf), "%dh ago", v.minutes_since_fetch / 60);
    g.setTextColor(EPD_BLACK);
    g.setCursor(g.width() - (int)strlen(tbuf) * EpdGfx::charW(1) - 2, 2);
    g.print(tbuf);

    g.drawHLine(0, 16, g.width(), EPD_BLACK);

    // --- device name (big) + model ---
    g.setTextSize(2);
    g.setTextColor(EPD_BLACK);
    g.setCursor(4, 21);
    g.print(d.device_name);
    // model abbreviation, truncated to fit (requirement 5.4: may be omitted)
    if (d.model_name[0]) {
        char m[26];
        strncpy(m, d.model_name, sizeof(m) - 1);
        m[sizeof(m) - 1] = '\0';
        g.setTextSize(1);
        g.setCursor(4, 40);
        g.print(m);
    }

    // --- temperature + bar ---
    g.setTextSize(1);
    g.setCursor(4, 52);
    g.printf("Temp %d", (int)d.temp);
    int dx = g.cursorX();
    g.drawCircle(dx + 1, 53, 1, EPD_BLACK); // degree symbol
    g.setCursor(dx + 5, 52);
    g.print("C");
    // bar 0..60C
    int bx = 90, bw = g.width() - bx - 6, bh = 8, by = 51;
    g.drawRect(bx, by, bw, bh, EPD_BLACK);
    int t = d.temp; if (t < 0) t = 0; if (t > 60) t = 60;
    int fillw = (bw - 2) * t / 60;
    g.fillRect(bx + 1, by + 1, fillw, bh - 2, EPD_BLACK);

    // --- life ---
    g.setCursor(4, 66);
    long days = (long)(d.power_on_hours / 24);
    if (days >= 365) g.printf("On %lldh (~%ldy)", (long long)d.power_on_hours, days / 365);
    else             g.printf("On %lldh (~%ldd)", (long long)d.power_on_hours, days);

    // --- bad sectors ---
    g.setCursor(4, 80);
    g.printf("Re:%lld Pe:%lld Un:%lld",
             (long long)d.realloc, (long long)d.pending, (long long)d.uncorrectable);

    // device_status raw (small, bottom-left)
    g.setCursor(4, 94);
    g.printf("status=%llu", (unsigned long long)d.device_status);

    if (v.stale) draw_stale(g);

    present(LAYOUT_DISK);
}

void display_show_message(const char* title, const char* body) {
    EpdGfx& g = s_gfx;
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
