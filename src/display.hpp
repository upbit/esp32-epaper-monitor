// display.hpp - business-facing display facade. Hides the GFX/driver details
// and owns the full-vs-partial refresh policy (requirement 5.2).
#pragma once

#include "esp_err.h"
#include "disks.hpp"

// Render-ready view of a single disk for the carousel.
struct DiskView
{
    const Disk *disk;
    int index; // 0-based
    int total;
    int minutes_since_fetch;
    bool stale;
};

esp_err_t display_init();

// Startup self-check page == WiFi info page (shared renderer).
// Pass last_fetch_min < 0 and disks < 0 to render the minimal boot variant.
void display_show_info(const char *ssid, const char *ip, const char *version,
                       int last_fetch_min, int disks_count, bool stale);

void display_show_disk(const DiskView &v);
void display_show_message(const char *title, const char *body);
void display_hibernate();

// Consecutive panel BUSY failures (for watchdog/restart policy).
int display_busy_fail();
