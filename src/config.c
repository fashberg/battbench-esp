#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

bb_config_t g_cfg;
charger_entry_t g_chargers[MAX_CHARGERS];
uint16_t g_boot;

static void get_str(nvs_handle_t h, const char *key, char *out, size_t size, const char *def)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, def, size);
    }
}

static uint16_t get_u16(nvs_handle_t h, const char *key, uint16_t def)
{
    uint16_t v;
    return nvs_get_u16(h, key, &v) == ESP_OK ? v : def;
}

void config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("bb", NVS_READWRITE, &h));
    get_str(h, "ssid", g_cfg.ssid, sizeof g_cfg.ssid, "");
    get_str(h, "pass", g_cfg.pass, sizeof g_cfg.pass, "");
    get_str(h, "host", g_cfg.hostname, sizeof g_cfg.hostname, "battbench-esp");
    get_str(h, "ap_pass", g_cfg.ap_pass, sizeof g_cfg.ap_pass, "battbench");
    get_str(h, "ble_addr", g_cfg.ble_addr, sizeof g_cfg.ble_addr, "");
    g_cfg.poll_ms = get_u16(h, "poll_ms", 2000);
    g_cfg.thin_secs = get_u16(h, "thin", 60);
    g_cfg.flash_log = get_u16(h, "flash", 1) != 0;
    g_cfg.led_gpio = get_u16(h, "led", 48);
    g_cfg.ble_on = get_u16(h, "ble_on", 1) != 0;

    size_t len = sizeof g_chargers;
    if (nvs_get_blob(h, "chargers", g_chargers, &len) != ESP_OK || len != sizeof g_chargers) {
        memset(g_chargers, 0, sizeof g_chargers);
    }
    g_boot = get_u16(h, "boot", 0) + 1;
    if (g_boot == 0) {
        g_boot = 1;
    }
    nvs_set_u16(h, "boot", g_boot);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "boot %u, ssid '%s', charger '%s'", g_boot, g_cfg.ssid, g_cfg.ble_addr);
}

esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("bb", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", g_cfg.ssid);
    nvs_set_str(h, "pass", g_cfg.pass);
    nvs_set_str(h, "host", g_cfg.hostname);
    nvs_set_str(h, "ap_pass", g_cfg.ap_pass);
    nvs_set_str(h, "ble_addr", g_cfg.ble_addr);
    nvs_set_u16(h, "poll_ms", g_cfg.poll_ms);
    nvs_set_u16(h, "thin", g_cfg.thin_secs);
    nvs_set_u16(h, "flash", g_cfg.flash_log);
    nvs_set_u16(h, "led", g_cfg.led_gpio);
    nvs_set_u16(h, "ble_on", g_cfg.ble_on);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

int charger_src(const char *addr, uint8_t model)
{
    int idx = -1, oldest = 0;
    for (int i = 0; i < MAX_CHARGERS && idx < 0; i++) {
        if (strcmp(g_chargers[i].addr, addr) == 0 && g_chargers[i].model == model) {
            idx = i;
        }
    }
    for (int i = 0; i < MAX_CHARGERS && idx < 0; i++) {
        if (g_chargers[i].addr[0] == 0) {
            idx = i;
        }
    }
    for (int i = 1; i < MAX_CHARGERS; i++) {
        if (g_chargers[i].last_boot < g_chargers[oldest].last_boot) {
            oldest = i;
        }
    }
    if (idx < 0) {
        idx = oldest;           // table full: the charger not seen for the longest time makes room
    }
    if (strcmp(g_chargers[idx].addr, addr) != 0 || g_chargers[idx].model != model ||
        g_chargers[idx].last_boot != g_boot) {
        strlcpy(g_chargers[idx].addr, addr, sizeof g_chargers[idx].addr);
        g_chargers[idx].model = model;
        g_chargers[idx].last_boot = g_boot;
        nvs_handle_t h;
        if (nvs_open("bb", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "chargers", g_chargers, sizeof g_chargers);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    return idx;
}
