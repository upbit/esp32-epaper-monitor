#include "scrutiny_fetch.hpp"
#include "disks.hpp"
#include "config.h"

#include <cstdlib>
#include <cstring>
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

static const char* TAG = "FETCH";

// --- small cJSON helpers (paths are fixed per Scrutiny v0.9.2 schema) ------
static const char* j_str(const cJSON* o, const char* key) {
    const cJSON* i = cJSON_GetObjectItemCaseSensitive(o, key);
    return (i && cJSON_IsString(i) && i->valuestring) ? i->valuestring : "";
}
static int64_t j_num(const cJSON* o, const char* key) {
    const cJSON* i = cJSON_GetObjectItemCaseSensitive(o, key);
    return (i && cJSON_IsNumber(i)) ? (int64_t)i->valuedouble : 0;
}
static int64_t j_attr_value(const cJSON* attrs, const char* num_key) {
    if (!attrs) return 0;
    const cJSON* a = cJSON_GetObjectItemCaseSensitive(attrs, num_key);
    return a ? j_num(a, "value") : 0;
}

// Stream the HTTP body into a growable buffer (requirement 4.5).
static char* http_get_body(const char* url, int* out_len, int* out_status) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = HTTP_FETCH_TIMEOUT_MS;
    cfg.method = HTTP_METHOD_GET;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return nullptr;

    char* buf = nullptr;
    int   len = 0;
    *out_status = -1;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return nullptr;
    }
    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    *out_status = status;
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return nullptr;
    }

    int cap = 8192;
    buf = (char*)malloc(cap);
    if (!buf) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return nullptr; }

    while (true) {
        if (len + 1025 > cap) {
            int ncap = cap * 2;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) { free(buf); buf = nullptr; len = 0; break; }
            buf = nb; cap = ncap;
        }
        int r = esp_http_client_read(cli, buf + len, 1024);
        if (r < 0) { ESP_LOGE(TAG, "read error"); free(buf); buf = nullptr; len = 0; break; }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(cli)) break;
            if (len > 0) break; // connection closed
        }
        len += r;
    }
    if (buf) buf[len] = '\0';

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    *out_len = len;
    return buf;
}

bool scrutiny_fetch_now() {
    char url[256];
    snprintf(url, sizeof(url), "%s/summary", SCRUTINY_API_BASE);
    ESP_LOGI(TAG, "GET %s", url);

    int len = 0, status = 0;
    char* body = http_get_body(url, &len, &status);
    if (!body) { disks_note_fetch_fail(); return false; }

    cJSON* root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        disks_note_fetch_fail();
        return false;
    }

    cJSON* data    = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* summary = data ? cJSON_GetObjectItemCaseSensitive(data, "summary") : nullptr;
    if (!summary || !cJSON_IsObject(summary)) {
        ESP_LOGE(TAG, "missing data.summary");
        cJSON_Delete(root);
        disks_note_fetch_fail();
        return false;
    }

    Disk* arr = (Disk*)calloc(MAX_DISKS, sizeof(Disk));
    if (!arr) { cJSON_Delete(root); disks_note_fetch_fail(); return false; }

    int n = 0;
    for (cJSON* it = summary->child; it && n < MAX_DISKS; it = it->next) {
        Disk& d = arr[n];
        strncpy(d.scrutiny_id, it->string ? it->string : "", sizeof(d.scrutiny_id) - 1);

        const cJSON* dev   = cJSON_GetObjectItemCaseSensitive(it, "device");
        const cJSON* smart = cJSON_GetObjectItemCaseSensitive(it, "smart");

        if (dev) {
            strncpy(d.device_name,   j_str(dev, "device_name"),   sizeof(d.device_name) - 1);
            strncpy(d.model_name,    j_str(dev, "model_name"),    sizeof(d.model_name) - 1);
            strncpy(d.serial_number, j_str(dev, "serial_number"), sizeof(d.serial_number) - 1);
        }
        if (smart) {
            d.temp           = (int32_t)j_num(smart, "temp");
            d.power_on_hours = j_num(smart, "power_on_hours");
            d.device_status  = (uint64_t)j_num(smart, "device_status");
            const cJSON* attrs = cJSON_GetObjectItemCaseSensitive(smart, "attrs");
            d.realloc       = j_attr_value(attrs, "5");
            d.pending       = j_attr_value(attrs, "197");
            d.uncorrectable = j_attr_value(attrs, "198");
        }
        d.health = disk_compute_health(d);
        ++n;
    }

    cJSON_Delete(root);
    disks_replace(arr, n);
    free(arr);

    ESP_LOGI(TAG, "fetched %d disk(s)", n);
    return true;
}
