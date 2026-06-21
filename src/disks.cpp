#include "disks.hpp"

#include <cstring>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static SemaphoreHandle_t s_mtx = nullptr;
static Disk s_disks[MAX_DISKS];
static int s_count = 0;
static int64_t s_last_fetch_sec = -1;
static int s_consec_fail = 0;

int64_t now_sec() { return esp_timer_get_time() / 1000000LL; }

DiskHealth disk_compute_health(const Disk &d)
{
    const bool bad_sectors = (d.realloc > 0) || (d.pending > 0) || (d.uncorrectable > 0);
    if (d.device_status != 0)
        return DiskHealth::FAIL;
    if (bad_sectors)
        return DiskHealth::WARN;
    return DiskHealth::OK;
}

void disks_init()
{
    if (!s_mtx)
        s_mtx = xSemaphoreCreateMutex();
}

static inline void lock()
{
    if (s_mtx)
        xSemaphoreTake(s_mtx, portMAX_DELAY);
}
static inline void unlock()
{
    if (s_mtx)
        xSemaphoreGive(s_mtx);
}

bool disks_replace(const Disk *arr, int n)
{
    if (!s_mtx)
        return false;
    if (n < 0)
        n = 0;
    if (n > MAX_DISKS)
        n = MAX_DISKS;

    lock();
    for (int i = 0; i < n; ++i)
    {
        s_disks[i] = arr[i];
        s_disks[i].health = disk_compute_health(s_disks[i]);
    }
    s_count = n;
    // Stable sort by device_name for a deterministic carousel order.
    std::stable_sort(s_disks, s_disks + s_count, [](const Disk &a, const Disk &b)
                     { return std::strcmp(a.device_name, b.device_name) < 0; });
    s_last_fetch_sec = now_sec();
    s_consec_fail = 0;
    unlock();
    return true;
}

int disks_snapshot(Disk *out, int max)
{
    lock();
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; ++i)
        out[i] = s_disks[i];
    unlock();
    return n;
}

void disks_note_fetch_ok()
{
    lock();
    s_consec_fail = 0;
    unlock();
}
void disks_note_fetch_fail()
{
    lock();
    ++s_consec_fail;
    unlock();
}

int disks_consec_fail()
{
    lock();
    int v = s_consec_fail;
    unlock();
    return v;
}
int disks_count()
{
    lock();
    int v = s_count;
    unlock();
    return v;
}

int64_t disks_last_fetch_sec()
{
    lock();
    int64_t v = s_last_fetch_sec;
    unlock();
    return v;
}
