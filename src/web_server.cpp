#include "web_server.hpp"
#include "disks.hpp"
#include "wifi_sta.hpp"
#include "config.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "HTTPD";
static httpd_handle_t s_server = nullptr;

static void chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    httpd_resp_sendstr_chunk(req, b);
}

// Minimal HTML-escape into caller buffer; returns dst.
static const char *esc(const char *s, char *dst, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; s && s[i] && j + 7 < cap; ++i)
    {
        char c = s[i];
        if (c == '<')
        {
            memcpy(dst + j, "&lt;", 4);
            j += 4;
        }
        else if (c == '>')
        {
            memcpy(dst + j, "&gt;", 4);
            j += 4;
        }
        else if (c == '&')
        {
            memcpy(dst + j, "&amp;", 5);
            j += 5;
        }
        else
            dst[j++] = c;
    }
    dst[j] = '\0';
    return dst;
}

static int last_fetch_min()
{
    int64_t lf = disks_last_fetch_sec();
    if (lf < 0)
        return -1;
    return (int)((now_sec() - lf) / 60);
}

static const char *health_str(DiskHealth h)
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
static const char *health_cls(DiskHealth h)
{
    switch (h)
    {
    case DiskHealth::OK:
        return "ok";
    case DiskHealth::WARN:
        return "warn";
    default:
        return "fail";
    }
}

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    Disk *disks = (Disk *)calloc(MAX_DISKS, sizeof(Disk));
    int n = disks ? disks_snapshot(disks, MAX_DISKS) : 0;
    int fmin = last_fetch_min();
    int fail = disks_consec_fail();
    bool stale = fail >= FETCH_FAIL_STALE_THRESHOLD;

    chunkf(req,
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>E-Paper Monitor</title><style>"
           ":root{color-scheme:dark}"
           "body{margin:0;background:#0f1216;color:#e6e6e6;font:14px/1.4 system-ui,Segoe UI,Roboto,sans-serif}"
           "header{padding:16px 20px;background:#161b22;border-bottom:1px solid #283040}"
           "h1{margin:0 0 6px;font-size:20px}"
           ".meta{color:#9aa4b2;font-size:13px}"
           ".stale{color:#ff5c5c;font-weight:700}"
           ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:14px;padding:20px}"
           ".card{background:#161b22;border:1px solid #283040;border-radius:10px;padding:14px}"
           ".row{display:flex;justify-content:space-between;align-items:center;margin:4px 0}"
           ".name{font-size:17px;font-weight:700}"
           ".sub{color:#9aa4b2;font-size:12px;word-break:break-all}"
           ".badge{padding:2px 10px;border-radius:12px;font-weight:700;font-size:12px}"
           ".ok{background:#143d28;color:#3ddc84}.warn{background:#4d3d12;color:#ffcc4d}.fail{background:#4d1717;color:#ff5c5c}"
           ".bar{height:10px;background:#283040;border-radius:6px;overflow:hidden;margin-top:4px}"
           ".bar>i{display:block;height:100%%;background:linear-gradient(90deg,#3ddc84,#ffcc4d,#ff5c5c)}"
           ".kv{display:flex;justify-content:space-between;color:#cdd5df;font-size:13px;margin:3px 0}"
           ".bad{color:#ff8a8a}"
           "</style></head><body>");

    chunkf(req, "<header><h1>E-Paper Monitor</h1><div class=meta>");
    chunkf(req, "SSID: %s &nbsp; IP: %s &nbsp; FW: %s (%s)<br>",
           wifi_ssid(), wifi_ip(), FW_VERSION, __DATE__);
    if (fmin < 0)
        chunkf(req, "Data: <span class=stale>not fetched yet</span>");
    else
        chunkf(req, "Last update: %dm ago &nbsp; Disks: %d", fmin, n);
    if (stale)
        chunkf(req, " &nbsp; <span class=stale>STALE (%d fails)</span>", fail);
    chunkf(req, "</div></header>");

    if (n == 0)
    {
        int next = fmin < 0 ? FETCH_INTERVAL_SEC : (FETCH_INTERVAL_SEC - (int)(now_sec() - disks_last_fetch_sec()));
        if (next < 0)
            next = 0;
        chunkf(req,
               "<div class=grid><div class=card><div class=name>Data not ready</div>"
               "<div class=sub>No disk data cached yet.</div>"
               "<div class=kv><span>Next fetch in</span><span>~%ds</span></div>"
               "<div class=kv><span>Consecutive fails</span><span>%d</span></div></div></div>",
               next, fail);
    }
    else
    {
        chunkf(req, "<div class=grid>");
        char m[64], s[48], nm[24], ty[16], cap[20];
        for (int i = 0; i < n; ++i)
        {
            const Disk &d = disks[i];
            int tw = d.temp;
            if (tw < 0)
                tw = 0;
            if (tw > 60)
                tw = 60;
            long days = (long)(d.power_on_hours / 24);
            chunkf(req, "<div class=card>"
                        "<div class=row><span class=name>%s</span><span class='badge %s'>%s</span></div>",
                   esc(d.device_name, nm, sizeof(nm)), health_cls(d.health), health_str(d.health));
            chunkf(req, "<div class=sub>%s</div><div class=sub>SN: %s</div>",
                   esc(d.model_name, m, sizeof(m)), esc(d.serial_number, s, sizeof(s)));
            chunkf(req, "<div class=kv><span>Type / Capacity</span><span>%s / %s</span></div>",
                   esc(d.device_type, ty, sizeof(ty)), esc(d.capacity_label, cap, sizeof(cap)));
            chunkf(req, "<div class=kv><span>Temp</span><span>%d&deg;C</span></div>"
                        "<div class=bar><i style='width:%d%%'></i></div>",
                   d.temp, tw * 100 / 60);
            if (days >= 365)
                chunkf(req, "<div class=kv><span>Power-on</span><span>%lldh (~%ldy)</span></div>",
                       (long long)d.power_on_hours, days / 365);
            else
                chunkf(req, "<div class=kv><span>Power-on</span><span>%lldh (~%ldd)</span></div>",
                       (long long)d.power_on_hours, days);
            chunkf(req, "<div class=kv><span>Bad sectors</span>"
                        "<span class=%s>Re %lld / Pe %lld / Un %lld</span></div>",
                   (d.realloc || d.pending || d.uncorrectable) ? "bad" : "",
                   (long long)d.realloc, (long long)d.pending, (long long)d.uncorrectable);
            chunkf(req, "<div class=kv><span>device_status</span><span>%llu</span></div></div>",
                   (unsigned long long)d.device_status);
        }
        chunkf(req, "</div>");
    }

    chunkf(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, nullptr);
    free(disks);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/disks
// ---------------------------------------------------------------------------
static esp_err_t h_api_disks(httpd_req_t *req)
{
    Disk *disks = (Disk *)calloc(MAX_DISKS, sizeof(Disk));
    int n = disks ? disks_snapshot(disks, MAX_DISKS) : 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "last_fetch_ts", (double)disks_last_fetch_sec());
    cJSON_AddNumberToObject(root, "uptime_s", (double)now_sec());
    cJSON_AddNumberToObject(root, "consec_fetch_fail", disks_consec_fail());
    cJSON *arr = cJSON_AddArrayToObject(root, "disks");
    for (int i = 0; i < n; ++i)
    {
        const Disk &d = disks[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "scrutiny_id", d.scrutiny_id);
        cJSON_AddStringToObject(o, "device_name", d.device_name);
        cJSON_AddStringToObject(o, "device_type", d.device_type);
        cJSON_AddStringToObject(o, "model_name", d.model_name);
        cJSON_AddStringToObject(o, "serial_number", d.serial_number);
        cJSON_AddStringToObject(o, "capacity_label", d.capacity_label);
        cJSON_AddNumberToObject(o, "capacity_bytes", (double)d.capacity_bytes);
        cJSON_AddNumberToObject(o, "temp", d.temp);
        cJSON_AddNumberToObject(o, "power_on_hours", (double)d.power_on_hours);
        cJSON_AddNumberToObject(o, "device_status", (double)d.device_status);
        cJSON_AddNumberToObject(o, "realloc", (double)d.realloc);
        cJSON_AddNumberToObject(o, "pending", (double)d.pending);
        cJSON_AddNumberToObject(o, "uncorrectable", (double)d.uncorrectable);
        cJSON_AddStringToObject(o, "health", health_str(d.health));
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    free(disks);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /healthz
// ---------------------------------------------------------------------------
static esp_err_t h_healthz(httpd_req_t *req)
{
    char b[200];
    snprintf(b, sizeof(b),
             "{\"status\":\"ok\",\"last_fetch_ts\":%lld,\"disks\":%d,\"uptime_s\":%lld,\"consec_fetch_fail\":%d}",
             (long long)disks_last_fetch_sec(), disks_count(),
             (long long)now_sec(), disks_consec_fail());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, b);
    return ESP_OK;
}

void web_server_start()
{
    if (s_server)
        return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_SERVER_PORT;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    if (httpd_start(&s_server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_uri_t root = {"/", HTTP_GET, h_root, nullptr};
    httpd_uri_t api_disks = {"/api/disks", HTTP_GET, h_api_disks, nullptr};
    httpd_uri_t healthz = {"/healthz", HTTP_GET, h_healthz, nullptr};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &api_disks);
    httpd_register_uri_handler(s_server, &healthz);
    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_SERVER_PORT);
}
