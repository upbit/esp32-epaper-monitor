// E-Paper Monitor (ESP-IDF) - Scrutiny disk-health carousel on a 2.13" SSD1675A.
//
// Task layout (requirement 7):
//   - app_main : startup orchestration (WiFi -> boot page -> first fetch)
//   - fetch_task   : performs Scrutiny fetches when notified
//   - display_task : disk carousel + WiFi info page + fetch triggering
//   - esp_http_server internal task : dashboard
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "config.h"
#include "disks.hpp"
#include "display.hpp"
#include "wifi_sta.hpp"
#include "scrutiny_fetch.hpp"
#include "web_server.hpp"

static const char *TAG = "APP";

static TaskHandle_t s_fetch_task = nullptr;

// --- fetch task: blocks until notified, then performs one fetch -----------
static void fetch_task(void *)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!wifi_is_connected())
        {
            ESP_LOGW(TAG, "fetch skipped: WiFi down");
            continue;
        }
        ESP_LOGI(TAG, "fetch triggered");
        scrutiny_fetch_now();
    }
}

static int compute_minutes(int64_t *out_lf)
{
    int64_t lf = disks_last_fetch_sec();
    if (out_lf)
        *out_lf = lf;
    if (lf < 0)
        return 0;
    return (int)((now_sec() - lf) / 60);
}

static bool compute_stale()
{
    int64_t lf = disks_last_fetch_sec();
    bool too_old = (lf >= 0) && ((now_sec() - lf) > 2LL * FETCH_INTERVAL_SEC);
    return too_old || (disks_consec_fail() >= FETCH_FAIL_STALE_THRESHOLD);
}

// --- display task: carousel + WiFi page + fetch triggering ----------------
static void display_task(void *)
{
    static Disk round[MAX_DISKS];
    int64_t last_stats = 0;

    for (;;)
    {
        // Periodic resource log (requirement 7.4 / 9).
        if (now_sec() - last_stats >= 60)
        {
            ESP_LOGI("STAT", "free_heap=%u disp_stack=%u",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            last_stats = now_sec();
        }

        // Watchdog: too many panel failures -> restart (requirement 7.3).
        if (display_busy_fail() >= 10)
        {
            ESP_LOGE(TAG, "panel failed repeatedly, restarting");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        int n = disks_snapshot(round, MAX_DISKS);
        bool stale = compute_stale();

        if (n == 0)
        {
            display_show_message("No disks", "No data reported. Retrying...");
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_PER_DISK_SECONDS * 1000));
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                int64_t lf;
                DiskView v;
                v.disk = &round[i];
                v.index = i;
                v.total = n;
                v.minutes_since_fetch = compute_minutes(&lf);
                v.stale = stale;
                display_show_disk(v);
                vTaskDelay(pdMS_TO_TICKS(DISPLAY_PER_DISK_SECONDS * 1000));
            }
            // WiFi info page after a full round (requirement 5.5).
            display_show_info(wifi_ssid(), wifi_ip(), FW_VERSION,
                              compute_minutes(nullptr), n, stale,
                              (disks_consec_fail() == 0) ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(DISPLAY_PER_DISK_SECONDS * 1000));
        }

        // Trigger a fetch if the interval elapsed (requirement 5.6).
        int64_t lf = disks_last_fetch_sec();
        bool due = (lf < 0) || ((now_sec() - lf) >= FETCH_INTERVAL_SEC);
        if (due && wifi_is_connected() && s_fetch_task)
        {
            xTaskNotifyGive(s_fetch_task);
        }
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== E-Paper Monitor %s (build %s %s) ===", FW_VERSION, __DATE__, __TIME__);
    ESP_LOGI(TAG, "cfg: SSID=%s base=%s per_disk=%ds interval=%ds",
             WIFI_SSID, SCRUTINY_API_BASE, DISPLAY_PER_DISK_SECONDS, FETCH_INTERVAL_SEC);

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    disks_init();

    if (display_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "display init failed");
    }

    // (1) WiFi
    wifi_start();
    display_show_message("E-Paper Monitor", "WiFi connecting...");
    while (!wifi_wait_connected(60000))
    {
        ESP_LOGW(TAG, "WiFi not up after 60s, still retrying");
        display_show_message("WiFi", "connecting... " WIFI_SSID);
    }

    web_server_start();

    // (2) First fetch (synchronous), so the boot page can report its result.
    display_show_message("E-Paper Monitor", "Fetching status...");
    bool first_ok = scrutiny_fetch_now();

    // (3) Boot self-check page with the fetch status (>= 5s), consistent with
    //     the carousel's info page.
    {
        static Disk boot_round[MAX_DISKS];
        int n = disks_snapshot(boot_round, MAX_DISKS);
        display_show_info(wifi_ssid(), wifi_ip(), FW_VERSION,
                          compute_minutes(nullptr), n, compute_stale(),
                          first_ok ? 1 : 0);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));

    // (4) Background tasks + carousel
    xTaskCreate(fetch_task, "fetch_task", 6144, nullptr, 5, &s_fetch_task);
    xTaskCreate(display_task, "display_task", 6144, nullptr, 4, nullptr);
}
