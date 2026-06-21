#include "scrutiny_fetch.hpp"
#include "disks.hpp"
#include "config.h"

#include <cstdlib>
#include <cstring>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

static const char *TAG = "FETCH";

// --- small cJSON helpers (paths are fixed per Scrutiny v0.9.2 schema) ------
static const char *j_str(const cJSON *o, const char *key)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return (i && cJSON_IsString(i) && i->valuestring) ? i->valuestring : "";
}
static int64_t j_num(const cJSON *o, const char *key)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return (i && cJSON_IsNumber(i)) ? (int64_t)i->valuedouble : 0;
}
static const cJSON *j_first(const cJSON *o, const char *a, const char *b,
                            const char *c = nullptr, const char *d = nullptr)
{
    if (!o)
        return nullptr;
    const char *keys[] = {a, b, c, d};
    for (const char *key : keys)
    {
        if (!key)
            continue;
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
        if (item)
            return item;
    }
    return nullptr;
}
static const char *j_first_str(const cJSON *o, const char *a, const char *b,
                               const char *c = nullptr, const char *d = nullptr)
{
    const cJSON *item = j_first(o, a, b, c, d);
    return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}
static uint64_t j_first_u64(const cJSON *o, const char *a, const char *b,
                            const char *c = nullptr, const char *d = nullptr)
{
    const cJSON *item = j_first(o, a, b, c, d);
    return (item && cJSON_IsNumber(item) && item->valuedouble > 0)
               ? (uint64_t)item->valuedouble
               : 0;
}
static int64_t j_attr_value(const cJSON *attrs, const char *num_key)
{
    if (!attrs)
        return 0;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(attrs, num_key);
    return a ? j_num(a, "value") : 0;
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

static void set_device_type(Disk &d, const char *raw)
{
    const char *src = raw && raw[0] ? raw : d.device_name;
    const char *label = "SATA";
    if (contains_ci(src, "nvme"))
        label = "NVMe";
    else if (contains_ci(src, "usb"))
        label = "USB";
    else if (contains_ci(src, "sas"))
        label = "SAS";
    else if (contains_ci(src, "scsi"))
        label = "SCSI";
    else if (contains_ci(src, "sat") || contains_ci(src, "ata"))
        label = "SATA";
    strncpy(d.device_type, label, sizeof(d.device_type) - 1);
}

static void set_capacity_label(Disk &d, const cJSON *dev, const cJSON *disk_root)
{
    const cJSON *cap = j_first(dev, "capacity", "size", "device_size", "disk_size");
    if (!cap)
        cap = j_first(disk_root, "capacity", "size", "device_size", "disk_size");

    if (cap && cJSON_IsString(cap) && cap->valuestring && cap->valuestring[0])
    {
        strncpy(d.capacity_label, cap->valuestring, sizeof(d.capacity_label) - 1);
        return;
    }

    d.capacity_bytes = 0;
    if (cap && cJSON_IsNumber(cap) && cap->valuedouble > 0)
    {
        d.capacity_bytes = (uint64_t)cap->valuedouble;
    }
    else
    {
        d.capacity_bytes = j_first_u64(dev, "capacity_bytes", "total_bytes", "bytes", nullptr);
        if (!d.capacity_bytes)
        {
            d.capacity_bytes = j_first_u64(disk_root, "capacity_bytes", "total_bytes", "bytes", nullptr);
        }
    }
    if (!d.capacity_bytes)
    {
        strncpy(d.capacity_label, "--", sizeof(d.capacity_label) - 1);
        return;
    }

    const char *unit = "B";
    double v = (double)d.capacity_bytes;
    if (v >= 1000.0 * 1000.0 * 1000.0 * 1000.0)
    {
        v /= 1000.0 * 1000.0 * 1000.0 * 1000.0;
        unit = "TB";
    }
    else if (v >= 1000.0 * 1000.0 * 1000.0)
    {
        v /= 1000.0 * 1000.0 * 1000.0;
        unit = "GB";
    }
    else if (v >= 1000.0 * 1000.0)
    {
        v /= 1000.0 * 1000.0;
        unit = "MB";
    }

    if (v >= 10.0)
        snprintf(d.capacity_label, sizeof(d.capacity_label), "%.0f %s", v, unit);
    else
        snprintf(d.capacity_label, sizeof(d.capacity_label), "%.1f %s", v, unit);
}

// Accumulator passed through the HTTP event handler.
struct BodyAccum
{
    char *buf;
    int len;
    int cap;
    bool oom;
};

// HTTP_EVENT_ON_DATA delivers fully chunk-decoded body bytes. Accumulating
// here (rather than hand-rolling esp_http_client_read) avoids chunked-decode
// edge cases that previously truncated/corrupted the JSON body.
static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA)
        return ESP_OK;
    BodyAccum *a = (BodyAccum *)evt->user_data;
    if (a->oom)
        return ESP_OK;
    if (a->len + evt->data_len + 1 > a->cap)
    {
        int ncap = a->cap ? a->cap : 8192;
        while (a->len + evt->data_len + 1 > ncap)
            ncap *= 2;
        char *nb = (char *)realloc(a->buf, ncap);
        if (!nb)
        {
            a->oom = true;
            return ESP_OK;
        }
        a->buf = nb;
        a->cap = ncap;
    }
    memcpy(a->buf + a->len, evt->data, evt->data_len);
    a->len += evt->data_len;
    return ESP_OK;
}

static char *http_get_body(const char *url, int *out_len, int *out_status)
{
    BodyAccum acc = {};

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = HTTP_FETCH_TIMEOUT_MS;
    cfg.method = HTTP_METHOD_GET;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli)
        return nullptr;

    *out_status = -1;
    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        free(acc.buf);
        esp_http_client_cleanup(cli);
        return nullptr;
    }

    int status = esp_http_client_get_status_code(cli);
    *out_status = status;
    int content_len = (int)esp_http_client_get_content_length(cli);
    esp_http_client_cleanup(cli);

    if (status != 200)
    {
        ESP_LOGE(TAG, "HTTP status %d", status);
        free(acc.buf);
        return nullptr;
    }
    if (acc.oom || !acc.buf)
    {
        ESP_LOGE(TAG, "body alloc failed (len=%d)", acc.len);
        free(acc.buf);
        return nullptr;
    }

    acc.buf[acc.len] = '\0';
    ESP_LOGI(TAG, "received %d bytes (Content-Length hdr=%d)", acc.len, content_len);
    *out_len = acc.len;
    return acc.buf;
}

bool scrutiny_fetch_now()
{
    char url[256];
    snprintf(url, sizeof(url), "%s/summary", SCRUTINY_API_BASE);
    ESP_LOGI(TAG, "GET %s", url);

    int len = 0, status = 0;
    char *body = http_get_body(url, &len, &status);
    if (!body)
    {
        disks_note_fetch_fail();
        return false;
    }

    ESP_LOGI(TAG, "HTTP %d, body len=%d, free heap=%lu",
             status, len, (unsigned long)esp_get_free_heap_size());
    // Log the start and tail of the body so a truncated/garbled payload is
    // immediately visible in the serial log.
    {
        char head[81];
        int hn = len < 80 ? len : 80;
        memcpy(head, body, hn);
        head[hn] = '\0';
        ESP_LOGI(TAG, "body head: %s", head);
        if (len > 80)
        {
            int ts = len - 80;
            ESP_LOGI(TAG, "body tail: %s", body + ts);
        }
    }

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        const char *eptr = cJSON_GetErrorPtr();
        if (eptr)
        {
            int off = (int)(eptr - body);
            // Print a window of bytes around the parse-error offset.
            int start = off - 32;
            if (start < 0)
                start = 0;
            int wlen = 64;
            if (start + wlen > len)
                wlen = len - start;
            char win[65];
            if (wlen < 0)
                wlen = 0;
            if (wlen > 64)
                wlen = 64;
            memcpy(win, body + start, wlen);
            win[wlen] = '\0';
            ESP_LOGE(TAG, "JSON parse failed at offset %d/%d, near: \"%s\"",
                     off, len, win);
            // Hex dump the bytes around the error to expose any non-printable
            // / injected bytes (e.g. leaked chunk framing) that %s would hide.
            char hex[64 * 3 + 1];
            int hp = 0;
            for (int i = 0; i < wlen && hp < (int)sizeof(hex) - 3; ++i)
            {
                hp += snprintf(hex + hp, sizeof(hex) - hp, "%02x ",
                               (unsigned char)body[start + i]);
            }
            ESP_LOGE(TAG, "hex @%d: %s", start, hex);
        }
        else
        {
            ESP_LOGE(TAG, "JSON parse failed (no error ptr), len=%d", len);
        }
        free(body);
        disks_note_fetch_fail();
        return false;
    }
    free(body);

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *summary = data ? cJSON_GetObjectItemCaseSensitive(data, "summary") : nullptr;
    if (!summary || !cJSON_IsObject(summary))
    {
        ESP_LOGE(TAG, "missing data.summary");
        cJSON_Delete(root);
        disks_note_fetch_fail();
        return false;
    }

    Disk *arr = (Disk *)calloc(MAX_DISKS, sizeof(Disk));
    if (!arr)
    {
        cJSON_Delete(root);
        disks_note_fetch_fail();
        return false;
    }

    int n = 0;
    for (cJSON *it = summary->child; it && n < MAX_DISKS; it = it->next)
    {
        Disk &d = arr[n];
        strncpy(d.scrutiny_id, it->string ? it->string : "", sizeof(d.scrutiny_id) - 1);

        const cJSON *dev = cJSON_GetObjectItemCaseSensitive(it, "device");
        const cJSON *smart = cJSON_GetObjectItemCaseSensitive(it, "smart");

        if (dev)
        {
            strncpy(d.device_name, j_str(dev, "device_name"), sizeof(d.device_name) - 1);
            strncpy(d.model_name, j_str(dev, "model_name"), sizeof(d.model_name) - 1);
            strncpy(d.serial_number, j_str(dev, "serial_number"), sizeof(d.serial_number) - 1);
            set_device_type(d, j_first_str(dev, "device_type", "type", "interface_type", "device_protocol"));
            set_capacity_label(d, dev, it);
            // device_status lives in the device object (not smart) in the
            // /api/summary payload; 0 == passed.
            d.device_status = (uint64_t)j_num(dev, "device_status");
        }
        else
        {
            set_device_type(d, nullptr);
            set_capacity_label(d, nullptr, it);
        }
        if (smart)
        {
            d.temp = (int32_t)j_num(smart, "temp");
            d.power_on_hours = j_num(smart, "power_on_hours");
            // Per-attribute SMART values (5/197/198) are not present in the
            // summary endpoint, only in the per-device detail; default to 0.
            const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(smart, "attrs");
            d.realloc = j_attr_value(attrs, "5");
            d.pending = j_attr_value(attrs, "197");
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
