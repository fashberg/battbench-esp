// Web page and JSON API.
//   GET  /                      status page with Wi-Fi / Bluetooth / storage settings
//   GET  /api/info              firmware, log id, chargers (index -> model, address), next sequence number
//   GET  /api/status            Wi-Fi, Bluetooth, storage, clock, live values of the connected charger
//   GET  /api/records?since=N&max=M   readings with sequence number > N: [seq, unix time or null, charger
//                               index, slot, packet hex]; "more": true if there are further ones; &fmt=bin: binary
//                               (format in store.h)
//   GET  /api/config            settings (passwords left out)      POST /api/config  JSON with changed fields
//   GET  /api/wifiscan          networks in reach                  GET  /api/blescan  SkyRC chargers seen
//   POST /api/blescan           new Bluetooth scan                 POST /api/time    {"t": unix} (if no SNTP)
//   POST /api/ble               {"pause": minutes} / {"on": false}: free the charger for the SkyRC app / BattBench
//   POST /api/command           {"hex": packet}: program / start / stop / history (see h_command); the reply of
//                               98 holds all its pages (up to 3 packets of 208 bytes) one after the other
//   POST /api/clear             clear the log                      POST /api/reboot
//   POST /api/ota               firmware image as body (OTA update)
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "ble.h"
#include "cJSON.h"
#include "charger.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "lwip/sockets.h"
#include "net.h"
#include "store.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define MAX_RECORDS 5000

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    if (msg) {
        cJSON_AddStringToObject(j, "msg", msg);
    }
    return send_json(req, j);
}

static cJSON *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static char fetch_ip[40];                       // who fetched last (BattBench)

static void add_time(cJSON *o, const char *key, uint16_t boot, uint32_t up10)
{
    double t;
    if (store_unix(boot, up10, &t)) {
        cJSON_AddNumberToObject(o, key, t);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_info(httpd_req_t *req)
{
    store_stats_t s;
    store_stats(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "fw", FW_VERSION);
    cJSON_AddStringToObject(j, "name", g_cfg.hostname);
    uint8_t m[6];
    char mac[13];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof mac, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    cJSON_AddStringToObject(j, "id", mac);     // same as the TXT item "id" of the mDNS service _battbench._tcp
    char id[12];
    snprintf(id, sizeof id, "%08lx", (unsigned long)s.logid);
    cJSON_AddStringToObject(j, "logid", id);
    cJSON_AddNumberToObject(j, "seq_next", s.seq_next);
    cJSON_AddNumberToObject(j, "boot", g_boot);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(j, "now", s.clock_set ? tv.tv_sec + tv.tv_usec / 1e6 : 0);
    ble_status_t b;
    ble_get_status(&b);
    cJSON_AddNumberToObject(j, "src", b.src);
    cJSON_AddBoolToObject(j, "connected", strcmp(b.state, "connected") == 0);
    cJSON *a = cJSON_AddArrayToObject(j, "chargers");
    for (int i = 0; i < MAX_CHARGERS; i++) {
        if (g_chargers[i].addr[0]) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddNumberToObject(c, "src", i);
            cJSON_AddStringToObject(c, "model", ch_model_name(g_chargers[i].model));
            cJSON_AddStringToObject(c, "addr", g_chargers[i].addr);
            cJSON_AddItemToArray(a, c);
        }
    }
    return send_json(req, j);
}

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    uint32_t now10 = store_uptime10();
    cJSON_AddStringToObject(j, "fw", FW_VERSION);
    cJSON_AddNumberToObject(j, "uptime", now10 / 10);
    cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    net_status_t n;
    net_get_status(&n);
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "ssid", g_cfg.ssid);
    cJSON_AddBoolToObject(w, "connected", n.sta_connected);
    cJSON_AddStringToObject(w, "ip", n.ip);
    cJSON_AddNumberToObject(w, "rssi", n.rssi);
    cJSON_AddBoolToObject(w, "ap", n.ap_on);
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    cJSON_AddStringToObject(w, "ps", ps == WIFI_PS_NONE ? "none" : "modem sleep");
    cJSON_AddStringToObject(w, "ap_ssid", n.ap_ssid);
    cJSON_AddStringToObject(w, "hostname", g_cfg.hostname);

    ble_status_t b;
    ble_get_status(&b);
    cJSON *bj = cJSON_AddObjectToObject(j, "ble");
    cJSON_AddStringToObject(bj, "state", b.state);
    cJSON_AddStringToObject(bj, "addr", b.addr);
    cJSON_AddStringToObject(bj, "name", b.name);
    cJSON_AddStringToObject(bj, "model", ch_model_name(b.model));
    cJSON_AddNumberToObject(bj, "mtu", b.mtu);
    cJSON_AddNumberToObject(bj, "itvl_ms", b.itvl_ms);
    cJSON_AddNumberToObject(bj, "connected_secs", b.connected_up10 ? (now10 - b.connected_up10) / 10 : 0);
    cJSON_AddNumberToObject(bj, "answers", b.answers);
    cJSON_AddNumberToObject(bj, "misses", b.misses);
    cJSON_AddNumberToObject(bj, "last_answer_secs", b.last_answer_up10 ? (now10 - b.last_answer_up10) / 10 : -1);
    cJSON_AddStringToObject(bj, "error", b.last_error);
    cJSON_AddBoolToObject(bj, "on", g_cfg.ble_on);
    cJSON_AddNumberToObject(bj, "paused_secs", b.paused_secs);

    store_stats_t s;
    store_stats(&s);
    cJSON *sj = cJSON_AddObjectToObject(j, "store");
    char id[12];
    snprintf(id, sizeof id, "%08lx", (unsigned long)s.logid);
    cJSON_AddStringToObject(sj, "logid", id);
    cJSON_AddNumberToObject(sj, "seq_next", s.seq_next);
    cJSON_AddNumberToObject(sj, "ram_cap", s.ram_cap);
    cJSON_AddNumberToObject(sj, "ram_count", s.ram_count);
    if (s.ram_count) {
        add_time(sj, "ram_first_t", g_boot, s.ram_first_up10);
    }
    cJSON_AddBoolToObject(sj, "flash_ok", s.flash_ok);
    cJSON_AddBoolToObject(sj, "flash_log", g_cfg.flash_log);
    cJSON_AddNumberToObject(sj, "flash_sectors", s.flash_sectors);
    cJSON_AddNumberToObject(sj, "flash_used", s.flash_used);
    if (s.flash_timed) {
        add_time(sj, "flash_first_t", s.flash_first_boot, s.flash_first_up10);
        cJSON_AddNumberToObject(sj, "flash_timed", s.flash_timed);
    }
    cJSON_AddNumberToObject(sj, "flash_recs", s.flash_recs);
    cJSON_AddNumberToObject(sj, "flash_bytes", s.flash_bytes);
    cJSON_AddNumberToObject(sj, "thin_secs", g_cfg.thin_secs);
    // fetching by BattBench: last time, and the readings it does not have yet
    cJSON_AddNumberToObject(sj, "fetched_seq", s.fetched_seq);
    cJSON_AddNumberToObject(sj, "ram_open", s.ram_open);
    cJSON_AddNumberToObject(sj, "flash_open", s.flash_open);
    cJSON_AddNumberToObject(sj, "fetch_secs", s.fetch_secs);
    if (s.fetch_t) {
        cJSON_AddNumberToObject(sj, "fetch_t", (double)(int64_t)s.fetch_t);
    }
    if (s.fetch_secs >= 0) {
        cJSON_AddStringToObject(sj, "fetch_ip", fetch_ip);
    }
    uint16_t ob;
    uint32_t ou, oc = 0;
    bool exact = true;
    bool open = store_open(&oc, &exact, &ob, &ou);
    cJSON_AddNumberToObject(sj, "open", oc);
    cJSON_AddBoolToObject(sj, "open_exact", exact);
    if (open) {
        add_time(sj, "open_first_t", ob, ou);
        cJSON_AddNumberToObject(sj, "open_first_secs", ob == g_boot ? (int)((now10 - ou) / 10) : -1);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddBoolToObject(j, "clock_ok", s.clock_set);
    cJSON_AddNumberToObject(j, "now", s.clock_set ? (double)tv.tv_sec : 0);

    cJSON *slots = cJSON_AddArrayToObject(j, "slots");
    for (int i = 0; b.src >= 0 && i < CH_SLOTS; i++) {
        rec_t r;
        ch_values_t v;
        if (!store_live(b.src, i, &r) || !ch_parse(r.model, r.raw, r.len, &v)) {
            cJSON_AddItemToArray(slots, cJSON_CreateNull());
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "age", (now10 - r.up10) / 10.0);
        cJSON_AddStringToObject(o, "state", ch_state_text(r.model, &v));
        cJSON_AddStringToObject(o, "type", ch_type_name(r.model, v.btype));
        cJSON_AddNumberToObject(o, "mv", v.mv);
        cJSON_AddNumberToObject(o, "ma", v.dir < 0 ? -v.ma : v.ma);
        cJSON_AddNumberToObject(o, "mah", v.mah);
        cJSON_AddNumberToObject(o, "res", v.res);
        cJSON_AddNumberToObject(o, "temp", v.temp_c);
        cJSON_AddNumberToObject(o, "secs", v.secs);
        cJSON_AddNumberToObject(o, "err", v.err);
        cJSON_AddStringToObject(o, "error", ch_error_text(r.model, &v));
        cJSON_AddItemToArray(slots, o);
    }
    return send_json(req, j);
}

static uint32_t query_u32(httpd_req_t *req, const char *key, uint32_t def)
{
    char q[128], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) {
        return strtoul(v, NULL, 10);
    }
    return def;
}

static bool query_is(httpd_req_t *req, const char *key, const char *val)
{
    char q[128], v[16];
    return httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
           httpd_query_key_value(q, key, v, sizeof v) == ESP_OK && strcmp(v, val) == 0;
}

static void peer_ip(httpd_req_t *req, char *out, size_t len)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof a;
    out[0] = 0;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &al) != 0) {
        return;
    }
    if (a.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, out, len);
    } else if (a.ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        const uint8_t *b = a6->sin6_addr.s6_addr;
        static const uint8_t v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, v4mapped, 12) == 0) {
            snprintf(out, len, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, out, len);
        }
    }
}

// Output of /api/records: plain, or deflated (zlib format, "Content-Encoding: deflate") when the client accepts it.
// The hex JSON shrinks ~5 times, and Wi-Fi next to Bluetooth is the bottleneck when BattBench catches up.
// fmt=bin: the readings coded as in the flash log (format in store.h), much smaller still.
typedef struct {
    httpd_req_t *req;
    tdefl_compressor *z;        // NULL: plain
    char *out;                  // deflated bytes waiting to be sent
    int out_len;
} rec_out_t;

enum { REC_BUF = 16384, OUT_CHUNK = 4096 };    // deflated output goes out in small parts: sent while the rest is compressed

static mz_bool rec_put(const void *data, int len, void *user)
{
    rec_out_t *o = user;
    while (len > 0) {
        int k = len < OUT_CHUNK - o->out_len ? len : OUT_CHUNK - o->out_len;
        memcpy(o->out + o->out_len, data, k);
        o->out_len += k;
        data = (const char *)data + k;
        len -= k;
        if (o->out_len == OUT_CHUNK) {
            httpd_resp_send_chunk(o->req, o->out, o->out_len);
            o->out_len = 0;
        }
    }
    return MZ_TRUE;
}

static void rec_emit(rec_out_t *o, const char *buf, int len, bool last)
{
    if (!o->z) {
        httpd_resp_send_chunk(o->req, buf, len);
        return;
    }
    tdefl_compress_buffer(o->z, buf, len, last ? TDEFL_FINISH : TDEFL_NO_FLUSH);
    if (last && o->out_len) {
        httpd_resp_send_chunk(o->req, o->out, o->out_len);
        o->out_len = 0;
    }
}

static bool accepts_deflate(httpd_req_t *req)
{
    char v[64];
    return httpd_req_get_hdr_value_str(req, "Accept-Encoding", v, sizeof v) == ESP_OK && strstr(v, "deflate");
}

static esp_err_t h_records(httpd_req_t *req)
{
    uint32_t since = query_u32(req, "since", 0);
    store_note_fetched(since);
    peer_ip(req, fetch_ip, sizeof fetch_ip);
    int max = query_u32(req, "max", 500);
    if (max < 1 || max > MAX_RECORDS) {
        max = MAX_RECORDS;
    }
    rec_t *recs = heap_caps_malloc(max * sizeof(rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *buf = heap_caps_malloc(2 * REC_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rec_out_t o = {.req = req, .out = buf + REC_BUF};
    if (recs && buf && accepts_deflate(req)) {
        o.z = heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!recs || !buf) {
        free(recs);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (o.z) {
        tdefl_init(o.z, rec_put, &o, 4 | TDEFL_GREEDY_PARSING_FLAG | TDEFL_WRITE_ZLIB_HEADER);   // fast, ~4 times smaller
    }
    bool more = false;
    int n = store_read(since, max, recs, &more);
    store_stats_t s;
    store_stats(&s);
    store_stream_t *st = query_is(req, "fmt", "bin") ?
        heap_caps_malloc(sizeof(store_stream_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    httpd_resp_set_type(req, st ? "application/octet-stream" : "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (o.z) {
        httpd_resp_set_hdr(req, "Content-Encoding", "deflate");
    }
    if (st) {
        uint8_t *b = (uint8_t *)buf;
        int len = store_bin_header(b, s.logid, s.seq_next, more, n);
        st->open = false;
        for (int i = 0; i < n; i++) {
            len += store_stream_put(st, &recs[i], b + len);
            if (len > REC_BUF - STORE_STREAM_MAX) {
                rec_emit(&o, buf, len, false);
                len = 0;
            }
        }
        b[len++] = 0xFF;
        rec_emit(&o, buf, len, true);
        free(st);
        free(o.z);
        free(buf);
        free(recs);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    // large chunks: every send waits for the radio, which Wi-Fi shares with Bluetooth
    int len = snprintf(buf, REC_BUF, "{\"logid\":\"%08lx\",\"seq_next\":%lu,\"more\":%s,\"records\":[",
                       (unsigned long)s.logid, (unsigned long)s.seq_next, more ? "true" : "false");
    for (int i = 0; i < n; i++) {
        const rec_t *r = &recs[i];
        char t[24] = "null", hex[2 * REC_RAW_MAX + 1];
        double ut;
        if (store_unix(r->boot, r->up10, &ut)) {
            snprintf(t, sizeof t, "%.1f", ut);
        }
        for (int k = 0; k < r->len; k++) {
            sprintf(hex + 2 * k, "%02x", r->raw[k]);
        }
        hex[2 * r->len] = 0;
        len += snprintf(buf + len, REC_BUF - len, "%s[%lu,%s,%u,%u,\"%s\"]", i ? "," : "", (unsigned long)r->seq, t,
                        r->src, r->slot, hex);
        if (len > REC_BUF - 160) {
            rec_emit(&o, buf, len, false);
            len = 0;
        }
    }
    len += snprintf(buf + len, REC_BUF - len, "]}");
    rec_emit(&o, buf, len, true);
    free(o.z);
    free(buf);
    free(recs);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ssid", g_cfg.ssid);
    cJSON_AddBoolToObject(j, "pass_set", g_cfg.pass[0] != 0);
    cJSON_AddStringToObject(j, "hostname", g_cfg.hostname);
    cJSON_AddBoolToObject(j, "ap_pass_set", g_cfg.ap_pass[0] != 0);
    cJSON_AddStringToObject(j, "ble_addr", g_cfg.ble_addr);
    cJSON_AddNumberToObject(j, "poll_ms", g_cfg.poll_ms);
    cJSON_AddNumberToObject(j, "thin_secs", g_cfg.thin_secs);
    cJSON_AddBoolToObject(j, "flash_log", g_cfg.flash_log);
    cJSON_AddNumberToObject(j, "led_gpio", g_cfg.led_gpio);
    return send_json(req, j);
}

static void delayed_restart(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void restart_soon(void)
{
    xTaskCreate(delayed_restart, "restart", 2048, NULL, 5, NULL);
}

static bool take_str(cJSON *j, const char *key, char *dst, size_t size)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size || strcmp(dst, v->valuestring) == 0) {
        return false;
    }
    strcpy(dst, v->valuestring);
    return true;
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    cJSON *j = read_body(req);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON expected");
    }
    bool wifi = false, ble = false;
    wifi |= take_str(j, "ssid", g_cfg.ssid, sizeof g_cfg.ssid);
    wifi |= take_str(j, "pass", g_cfg.pass, sizeof g_cfg.pass);
    wifi |= take_str(j, "hostname", g_cfg.hostname, sizeof g_cfg.hostname);
    wifi |= take_str(j, "ap_pass", g_cfg.ap_pass, sizeof g_cfg.ap_pass);
    ble |= take_str(j, "ble_addr", g_cfg.ble_addr, sizeof g_cfg.ble_addr);
    cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "poll_ms")) && v->valueint >= 1000 && v->valueint <= 60000) {
        g_cfg.poll_ms = v->valueint;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "thin_secs")) && v->valueint >= 0 && v->valueint <= 600) {
        g_cfg.thin_secs = v->valueint;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "flash_log"))) {
        g_cfg.flash_log = cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "led_gpio")) && v->valueint >= 0 && v->valueint <= 48 &&
        v->valueint != g_cfg.led_gpio) {
        g_cfg.led_gpio = v->valueint;
        wifi = true;            // takes effect after a restart
    }
    cJSON_Delete(j);
    if (config_save() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving failed");
    }
    if (ble) {
        ble_reconnect();
    }
    if (wifi) {
        restart_soon();
        return send_ok(req, "restart");
    }
    return send_ok(req, NULL);
}

static esp_err_t h_wifiscan(httpd_req_t *req)
{
    wifi_ap_record_t *aps = calloc(20, sizeof(wifi_ap_record_t));
    int n = net_scan(aps, 20);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(a, o);
    }
    free(aps);
    return send_json(req, a);
}

static esp_err_t h_blescan_get(httpd_req_t *req)
{
    ble_seen_t seen[BLE_SEEN_MAX];
    int n = ble_get_seen(seen, BLE_SEEN_MAX);
    uint32_t now10 = store_uptime10();
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "addr", seen[i].addr);
        cJSON_AddStringToObject(o, "name", seen[i].name);
        // generic names ("#Charger-…") do not tell the model: then the one found out when it was connected
        uint8_t model = seen[i].model;
        for (int k = 0; !model && k < MAX_CHARGERS; k++) {
            if (strcasecmp(g_chargers[k].addr, seen[i].addr) == 0) {
                model = g_chargers[k].model;
            }
        }
        cJSON_AddStringToObject(o, "model", model ? ch_model_name(model) : seen[i].generic ? "SkyRC" : "");
        cJSON_AddNumberToObject(o, "rssi", seen[i].rssi);
        cJSON_AddNumberToObject(o, "age", (now10 - seen[i].up10) / 10);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, a);
}

// {"pause": minutes} (0 = end the pause) or {"on": true / false} (kept over a restart)
static esp_err_t h_ble(httpd_req_t *req)
{
    cJSON *j = read_body(req);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON expected");
    }
    cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "pause")) && v->valueint >= 0 && v->valueint <= 1440) {
        ble_pause(v->valueint);
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "on")) && cJSON_IsTrue(v) != g_cfg.ble_on) {
        g_cfg.ble_on = cJSON_IsTrue(v);
        config_save();
        ble_pause(0);
    }
    cJSON_Delete(j);
    return send_ok(req, NULL);
}

// {"hex": "0f2a94..."}: one packet to the charger, answer {"ok": true, "reply": "0f0494..."} (reply "" = none).
// Only what BattBench needs for programs, stop and the stored curve; the checksum must be right.
//   MC5000: 94 program a slot (starts it when a battery is in), FE stop, 98 voltage history of a slot,
//   5F slot summary (task, battery type, charge / discharge current), 96 / 95 read / set the system settings
//   (sounds, backlight, screen timeout; the byte after 96 / 95 is not used, BattBench sends 04)
static const uint8_t ALLOWED[] = {0x94, 0xFE, 0x98, 0x5F, 0x96, 0x95};

static esp_err_t h_command(httpd_req_t *req)
{
    cJSON *j = read_body(req);
    cJSON *h = j ? cJSON_GetObjectItem(j, "hex") : NULL;
    uint8_t p[64];
    int n = 0;
    const char *s = cJSON_IsString(h) ? h->valuestring : "";
    while (s[0] && s[1] && n < (int)sizeof p) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) {
            break;
        }
        p[n++] = v;
        s += 2;
    }
    cJSON_Delete(j);
    // and always for one slot (mask 1, 2, 4 or 8): FE 00 stops all slots, 94 00 programs all of them, and with
    // several bits the charger only takes the lowest one
    bool ok = n >= 5 && p[0] == 0x0F && p[1] + 2 == n && memchr(ALLOWED, p[2], sizeof ALLOWED) &&
              (p[3] == 1 || p[3] == 2 || p[3] == 4 || p[3] == 8);
    uint8_t cs = 0;
    for (int i = 2; i < n - 1; i++) {
        cs += p[i];
    }
    if (!ok || cs != p[n - 1]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "packet not allowed or broken");
    }
    // static: up to 3 history pages do not fit on the stack of the web server's task (one request at a time)
    static uint8_t reply[3 * 244];
    static char hex[2 * sizeof reply + 1];
    int r = ble_command(p, n, reply, sizeof reply, p[2] == 0x98 ? 4000 : 2000);
    if (r < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "charger not connected");
    }
    for (int i = 0; i < r; i++) {
        sprintf(hex + 2 * i, "%02x", reply[i]);
    }
    hex[2 * r] = 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "reply", hex);
    return send_json(req, o);
}

static esp_err_t h_blescan_post(httpd_req_t *req)
{
    ble_scan_request();
    return send_ok(req, NULL);
}

static esp_err_t h_time(httpd_req_t *req)
{
    cJSON *j = read_body(req);
    cJSON *t = j ? cJSON_GetObjectItem(j, "t") : NULL;
    bool set = false;
    // SNTP wins; BattBench sets the clock only while SNTP has not (no internet in the network)
    if (cJSON_IsNumber(t) && t->valuedouble > 1.7e9 && !store_clock_ok()) {
        struct timeval tv = {.tv_sec = (time_t)t->valuedouble,
                             .tv_usec = (suseconds_t)((t->valuedouble - (time_t)t->valuedouble) * 1e6)};
        settimeofday(&tv, NULL);
        store_clock_set();
        set = true;
        ESP_LOGI(TAG, "clock set by %s", "a client");
    }
    cJSON_Delete(j);
    return send_ok(req, set ? "set" : "unchanged");
}

static esp_err_t h_clear(httpd_req_t *req)
{
    store_clear();
    return send_ok(req, NULL);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    restart_soon();
    return send_ok(req, NULL);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!p || esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    char *buf = malloc(4096);
    int left = req->content_len;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0 || esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf);
            esp_ota_abort(h);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
        }
        left -= r;
    }
    free(buf);
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(p) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
    }
    ESP_LOGW(TAG, "firmware updated, restarting");
    restart_soon();
    return send_ok(req, "restart");
}

void web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 20;
    c.stack_size = 8192;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    httpd_handle_t s;
    if (httpd_start(&s, &c) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, h_index, NULL},
        {"/api/info", HTTP_GET, h_info, NULL},
        {"/api/status", HTTP_GET, h_status, NULL},
        {"/api/records", HTTP_GET, h_records, NULL},
        {"/api/config", HTTP_GET, h_config_get, NULL},
        {"/api/config", HTTP_POST, h_config_post, NULL},
        {"/api/wifiscan", HTTP_GET, h_wifiscan, NULL},
        {"/api/blescan", HTTP_GET, h_blescan_get, NULL},
        {"/api/blescan", HTTP_POST, h_blescan_post, NULL},
        {"/api/ble", HTTP_POST, h_ble, NULL},
        {"/api/command", HTTP_POST, h_command, NULL},
        {"/api/time", HTTP_POST, h_time, NULL},
        {"/api/clear", HTTP_POST, h_clear, NULL},
        {"/api/reboot", HTTP_POST, h_reboot, NULL},
        {"/api/ota", HTTP_POST, h_ota, NULL},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(s, &uris[i]);
    }
    ESP_LOGI(TAG, "web server on port %d", c.server_port);
}
