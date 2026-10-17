#include "net.h"

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "store.h"

static const char *TAG = "net";

#define AP_OFF_AFTER 30         // s connected before the own access point goes off
#define AP_ON_AFTER 30          // s without a connection before it comes back

static esp_netif_t *sta, *ap;
static volatile bool connected, ap_on, scanning;
static volatile uint32_t since_up10;            // connected / disconnected since
static char ip[16], ap_ssid[33];
static unsigned retries;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_cfg.ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (connected) {
            ESP_LOGW(TAG, "Wi-Fi disconnected");
            since_up10 = store_uptime10();
        }
        connected = false;
        ip[0] = 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "connected, IP %s", ip);
        connected = true;
        since_up10 = store_uptime10();
        retries = 0;
    }
}

static void on_sntp(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock set by SNTP");
    store_clock_set();
}

static void set_ap(bool on)
{
    if (on == ap_on) {
        return;
    }
    ap_on = on;
    esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    ESP_LOGI(TAG, "access point %s %s", ap_ssid, on ? "on" : "off");
}

// Access point on / off, reconnect every 10 s.
static void net_task(void *arg)
{
    uint32_t last_try = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = store_uptime10();
        if (connected) {
            if (ap_on && now - since_up10 > AP_OFF_AFTER * 10) {
                set_ap(false);
            }
            continue;
        }
        if (!ap_on && (!g_cfg.ssid[0] || now - since_up10 > AP_ON_AFTER * 10)) {
            set_ap(true);
        }
        if (g_cfg.ssid[0] && !scanning && now - last_try > 100) {
            last_try = now;
            retries++;
            esp_wifi_connect();
        }
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, g_cfg.hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof ap_ssid, "BattBench-%02X%02X", mac[4], mac[5]);
    wifi_config_t apc = {0};
    strlcpy((char *)apc.ap.ssid, ap_ssid, sizeof apc.ap.ssid);
    apc.ap.ssid_len = strlen(ap_ssid);
    apc.ap.channel = 1;
    apc.ap.max_connection = 4;
    if (strlen(g_cfg.ap_pass) >= 8) {
        strlcpy((char *)apc.ap.password, g_cfg.ap_pass, sizeof apc.ap.password);
        apc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        apc.ap.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config_t stc = {0};
    strlcpy((char *)stc.sta.ssid, g_cfg.ssid, sizeof stc.sta.ssid);
    strlcpy((char *)stc.sta.password, g_cfg.pass, sizeof stc.sta.password);
    stc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    stc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    stc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ap_on = !g_cfg.ssid[0];             // with a network: the access point only if it cannot be reached
    ESP_ERROR_CHECK(esp_wifi_set_mode(ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    esp_wifi_set_config(WIFI_IF_AP, &apc);
    esp_wifi_set_config(WIFI_IF_STA, &stc);
    ESP_ERROR_CHECK(esp_wifi_start());
    // No modem sleep: with it, answers took ~650 ms (ping) and large responses broke off. Falls back to modem sleep
    // if the coexistence with Bluetooth does not allow it.
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi power save off not possible (%s), modem sleep", esp_err_to_name(ps));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);   // Bluetooth only asks for a status twice a second

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = on_sntp;
    esp_netif_sntp_init(&sc);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_cfg.hostname);
        mdns_instance_name_set("BattBench-ESP");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        // BattBench finds the bridge by this service; id = MAC, stays the same when the host name changes
        char id[13];
        uint8_t m[6];
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        mdns_txt_item_t txt[] = {{"id", id}, {"fw", FW_VERSION}, {"path", "/api/info"}};
        mdns_service_add(NULL, "_battbench", "_tcp", 80, txt, 3);
    }
    xTaskCreate(net_task, "net", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Wi-Fi '%s', access point %s", g_cfg.ssid, ap_ssid);
}

void net_get_status(net_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->sta_connected = connected;
    strcpy(s->ip, ip);
    s->ap_on = ap_on;
    strcpy(s->ap_ssid, ap_ssid);
    s->retries = retries;
    wifi_ap_record_t r;
    if (connected && esp_wifi_sta_get_ap_info(&r) == ESP_OK) {
        s->rssi = r.rssi;
    }
}

int net_scan(wifi_ap_record_t *out, int max)
{
    scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = max;
        esp_wifi_scan_get_ap_records(&num, out);
        n = num;
    }
    scanning = false;
    return n;
}
