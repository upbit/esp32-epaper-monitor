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

// ---------------------------------------------------------------------------
// GET /  -- modern card UI. Served as a static shell; data is fetched
//           client-side from /api/disks and rendered as responsive cards.
// ---------------------------------------------------------------------------
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>E-Paper Monitor</title><style>"
    ":root{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;color:#dfe7f2;"
    "font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif;"
    "background:radial-gradient(1200px 800px at 80% -10%,#10243a 0%,#070b12 55%,#04060a 100%)}"
    "header{padding:18px 22px;display:flex;flex-wrap:wrap;gap:10px 22px;align-items:center;"
    "border-bottom:1px solid #15324d}"
    "h1{margin:0;font-size:20px;font-weight:800;letter-spacing:.5px;"
    "background:linear-gradient(90deg,#5ad1ff,#7b8cff);-webkit-background-clip:text;background-clip:text;color:transparent}"
    ".meta{color:#7e91ad;font-size:13px;display:flex;flex-wrap:wrap;gap:4px 16px}"
    ".meta b{color:#aec2dc;font-weight:600}"
    ".pill{margin-left:auto;font-size:12px;font-weight:700;padding:4px 12px;border-radius:999px;"
    "border:1px solid #1d4a3a;background:#0c2a1f;color:#3ddc84}"
    ".pill.stale{border-color:#5a2230;background:#2a1016;color:#ff6b81}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:18px;padding:22px;max-width:1500px;margin:0 auto}"
    ".card{position:relative;border-radius:18px;padding:18px;overflow:hidden;"
    "background:linear-gradient(180deg,rgba(20,32,48,.85),rgba(10,16,26,.92));"
    "border:1px solid #163251;box-shadow:0 10px 30px -12px #000,inset 0 0 0 1px rgba(90,209,255,.04)}"
    ".card::before{content:'';position:absolute;inset:0;border-radius:18px;padding:1px;pointer-events:none;"
    "background:linear-gradient(140deg,rgba(90,209,255,.35),transparent 40%);"
    "-webkit-mask:linear-gradient(#000 0 0) content-box,linear-gradient(#000 0 0);"
    "-webkit-mask-composite:xor;mask-composite:exclude}"
    ".top{display:flex;align-items:center;gap:14px}"
    ".ico{width:64px;height:64px;flex:0 0 64px;border-radius:14px;display:grid;place-items:center;"
    "background:radial-gradient(60% 60% at 50% 35%,#16334f,#0b1726);"
    "border:1px solid #1d3f60;box-shadow:0 0 18px -4px rgba(90,209,255,.5)}"
    ".ico svg{width:38px;height:38px}"
    ".hd{min-width:0;flex:1}"
    ".tline{display:flex;align-items:center;gap:8px}"
    ".type{font-size:11px;font-weight:700;letter-spacing:.6px;color:#8fb6e8;"
    "padding:3px 9px;border-radius:8px;background:#0e2236;border:1px solid #1c3d5c}"
    ".badge{margin-left:auto;font-size:11px;font-weight:800;letter-spacing:.5px;padding:3px 10px;border-radius:999px}"
    ".badge.ok{background:#0c2a1f;color:#3ddc84;border:1px solid #1d4a3a}"
    ".badge.warn{background:#2e2410;color:#ffcc4d;border:1px solid #5a4715}"
    ".badge.fail{background:#2a1016;color:#ff6b81;border:1px solid #5a2230}"
    ".dev{font-size:40px;font-weight:800;line-height:1;margin:6px 0 2px;"
    "background:linear-gradient(180deg,#eaf4ff,#9fc4ee);-webkit-background-clip:text;background-clip:text;color:transparent}"
    ".lbl{font-size:10px;letter-spacing:1.2px;color:#5f7693;font-weight:700}"
    ".model{font-size:13px;color:#b8cae0;word-break:break-all}"
    ".sn{font-size:11px;color:#5f7693;word-break:break-all;margin-top:2px}"
    ".metrics{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:14px}"
    ".m{border-radius:14px;padding:12px 14px;background:#0b1726;border:1px solid #163251}"
    ".m .mh{display:flex;align-items:center;gap:7px;font-size:10px;font-weight:700;letter-spacing:1px;color:#6f87a6}"
    ".m .mh svg{width:15px;height:15px}"
    ".m.cap .mh{color:#7fc7ff}.m.temp .mh{color:#5fe0a8}"
    ".big{font-size:30px;font-weight:800;line-height:1;margin-top:7px;color:#eaf4ff}"
    ".big u{font-size:13px;font-weight:700;text-decoration:none;color:#8aa3c2;margin-left:3px}"
    ".pwr{display:flex;align-items:center;gap:9px;margin-top:13px;padding-top:12px;border-top:1px solid #14283f}"
    ".pwr svg{width:18px;height:18px;color:#7e91ad}"
    ".pwr .lbl{margin-right:auto}"
    ".pwr b{font-size:16px;font-weight:800;color:#eaf4ff}"
    ".pwr small{color:#6f87a6}"
    ".sect{font-size:11px;color:#7e91ad;display:flex;justify-content:space-between;margin-top:11px}"
    ".sect .bad{color:#ff8a8a;font-weight:700}"
    ".empty{padding:60px 22px;text-align:center;color:#7e91ad}"
    ".empty .e1{font-size:18px;font-weight:700;color:#aec2dc;margin-bottom:6px}"
    "@media(max-width:420px){.metrics{grid-template-columns:1fr}.dev{font-size:34px}}"
    "</style></head><body>"
    "<header><h1>E-PAPER MONITOR</h1>"
    "<div class=meta id=meta></div><span class=pill id=pill></span></header>"
    "<div class=grid id=grid></div>"
    "<script>";

static const char PAGE_SCRIPT[] =
    "const ICON_NVME='<svg viewBox=\"0 0 24 24\" fill=none stroke=\"#5ad1ff\" stroke-width=1.6><rect x=3 y=7 width=18 height=10 rx=2/><path d=\"M7 7v10M11 7v10M15 7v10\"/><circle cx=18.5 cy=12 r=1 fill=\"#5ad1ff\"/></svg>';"
    "const ICON_HDD='<svg viewBox=\"0 0 24 24\" fill=none stroke=\"#5ad1ff\" stroke-width=1.6><rect x=3 y=4 width=18 height=16 rx=3/><circle cx=12 cy=12 r=5/><circle cx=12 cy=12 r=1.4 fill=\"#5ad1ff\"/><path d=\"M15.5 15.5l2.2 2.2\"/></svg>';"
    "const ICON_CAP='<svg viewBox=\"0 0 24 24\" fill=none stroke=currentColor stroke-width=1.6><ellipse cx=12 cy=6 rx=7 ry=2.6/><path d=\"M5 6v12c0 1.4 3.1 2.6 7 2.6s7-1.2 7-2.6V6M5 12c0 1.4 3.1 2.6 7 2.6s7-1.2 7-2.6\"/></svg>';"
    "const ICON_TEMP='<svg viewBox=\"0 0 24 24\" fill=none stroke=currentColor stroke-width=1.6><path d=\"M14 14.8V5a2 2 0 1 0-4 0v9.8a4 4 0 1 0 4 0z\"/><circle cx=12 cy=17 r=1.4 fill=currentColor/></svg>';"
    "const ICON_CLK='<svg viewBox=\"0 0 24 24\" fill=none stroke=currentColor stroke-width=1.6><circle cx=12 cy=12 r=9/><path d=\"M12 7v5l3.5 2\"/></svg>';"
    "function esc(s){return (s==null?'':String(s)).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));}"
    "function isNvme(d){return /nvme/i.test((d.device_type||'')+' '+(d.device_name||'')+' '+(d.model_name||''));}"
    "function splitVal(s){var m=String(s||'').trim().match(/^([0-9.]+)\\s*(.*)$/);return m?[m[1],m[2]]:[s||'--',''];}"
    "function powerOn(h){h=Number(h)||0;var d=Math.floor(h/24);if(d>=365){return [(h/8760).toFixed(1)+'y','('+h.toLocaleString()+' h)'];}return [d+'d','('+h.toLocaleString()+' h)'];}"
    "function card(d){var cap=splitVal(d.capacity_label);var hc=(d.health||'OK').toLowerCase();var hc2=hc=='ok'?'ok':(hc=='warn'?'warn':'fail');"
    "var po=powerOn(d.power_on_hours);var bad=(d.realloc||d.pending||d.uncorrectable);"
    "return '<div class=card><div class=top>'+"
    "'<div class=ico>'+(isNvme(d)?ICON_NVME:ICON_HDD)+'</div>'+"
    "'<div class=hd><div class=tline><span class=type>'+esc(d.device_type||(isNvme(d)?'NVMe':'HDD'))+'</span>'+"
    "'<span class=\"badge '+hc2+'\">'+esc(d.health||'OK')+'</span></div>'+"
    "'<div class=dev>'+esc(d.device_name||'disk')+'</div>'+"
    "'<div class=lbl>MODEL</div><div class=model>'+esc(d.model_name||'-')+'</div>'+"
    "(d.serial_number?'<div class=sn>SN '+esc(d.serial_number)+'</div>':'')+'</div></div>'+"
    "'<div class=metrics>'+"
    "'<div class=\"m cap\"><div class=mh>'+ICON_CAP+'CAPACITY</div><div class=big>'+esc(cap[0])+'<u>'+esc(cap[1])+'</u></div></div>'+"
    "'<div class=\"m temp\"><div class=mh>'+ICON_TEMP+'SMART TEMP</div><div class=big>'+esc(d.temp)+'<u>\\u00b0C</u></div></div>'+"
    "'</div>'+"
    "'<div class=pwr>'+ICON_CLK+'<span class=lbl>POWER-ON TIME</span><b>'+po[0]+'</b> <small>'+po[1]+'</small></div>'+"
    "'<div class=sect><span>device_status '+esc(d.device_status)+'</span>'+"
    "'<span class='+(bad?'class=bad':'')+'>Re '+(d.realloc|0)+' / Pe '+(d.pending|0)+' / Un '+(d.uncorrectable|0)+'</span></div>'+"
    "'</div>';}"
    "function fmtAge(s){if(s<0)return 'never';var m=Math.floor(s/60);if(m<60)return m+'m ago';return Math.floor(m/60)+'h '+(m%60)+'m ago';}"
    "async function refresh(){try{var r=await fetch('/api/disks',{cache:'no-store'});var j=await r.json();"
    "var lf=j.last_fetch_ts,up=j.uptime_s,age=lf<0?-1:(up-lf),fail=j.consec_fetch_fail||0;"
    "var ds=j.disks||[];"
    "document.getElementById('meta').innerHTML="
    "'<span>SSID <b>'+esc(SSID)+'</b></span><span>IP <b>'+esc(IP)+'</b></span>'+"
    "'<span>FW <b>'+esc(FW)+'</b></span><span>Disks <b>'+ds.length+'</b></span>'+"
    "'<span>Updated <b>'+fmtAge(age)+'</b></span>';"
    "var pill=document.getElementById('pill');var stale=fail>=STALE_TH||age<0;"
    "pill.className='pill'+(stale?' stale':'');pill.textContent=stale?('STALE'+(fail?' ('+fail+' fails)':'')):'LIVE';"
    "var g=document.getElementById('grid');"
    "g.innerHTML=ds.length?ds.map(card).join(''):"
    "'<div class=empty><div class=e1>No disk data</div>Waiting for the next Scrutiny fetch...</div>';"
    "}catch(e){document.getElementById('pill').textContent='OFFLINE';}}"
    "refresh();setInterval(refresh,15000);"
    "</script></body></html>";

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    // Inject runtime constants the script needs (escaped for JS string context).
    char ssid[64], ip[48];
    chunkf(req,
           "const SSID=\"%s\";const IP=\"%s\";const FW=\"%s (%s)\";const STALE_TH=%d;",
           esc(wifi_ssid(), ssid, sizeof(ssid)),
           esc(wifi_ip(), ip, sizeof(ip)),
           FW_VERSION, __DATE__, FETCH_FAIL_STALE_THRESHOLD);

    httpd_resp_sendstr_chunk(req, PAGE_SCRIPT);
    httpd_resp_sendstr_chunk(req, nullptr);
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
