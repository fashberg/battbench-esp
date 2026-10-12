// Status on the RGB LED of the DevKitC-1 (WS2812; GPIO48 on board v1.0, GPIO38 on v1.1, setting led_gpio).
//   blue                     Bluetooth connected, short dark blink = a reading came in
//   blue / green             all tasks done (every occupied slot finished)
//   blue / red               storage nearly full and BattBench has not fetched the oldest readings
//   red                      error: charger not found / not answering, or a slot reports an error
//   orange                   Wi-Fi error (not connected, only the own access point)
//   blue|green|red / orange  the same, plus Wi-Fi error (alternating)
//   blue, slowly pulsing     looking for the charger, Wi-Fi fine
//   violet                   Bluetooth paused or switched off (charger free for the SkyRC app / BattBench)
#include "led.h"

#include <math.h>
#include <string.h>
#include "ble.h"
#include "charger.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "net.h"
#include "store.h"

static const char *TAG = "led";
static led_strip_handle_t strip;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t OFF = {0, 0, 0}, BLUE = {0, 0, 255}, GREEN = {0, 255, 0}, RED = {255, 0, 0},
                   ORANGE = {255, 70, 0}, VIOLET = {150, 0, 255};
#define BRIGHT 0.10f

static void show(rgb_t c, float level)
{
    level *= BRIGHT;
    led_strip_set_pixel(strip, 0, c.r * level, c.g * level, c.b * level);
    led_strip_refresh(strip);
}

// Slots of the connected charger: all occupied ones done / any error.
static void slot_summary(int src, bool *all_done, bool *error)
{
    int occupied = 0, done = 0;
    *error = false;
    uint32_t now = store_uptime10();
    for (int i = 0; src >= 0 && i < CH_SLOTS; i++) {
        rec_t r;
        ch_values_t v;
        if (!store_live(src, i, &r) || now - r.up10 > 600 || !ch_parse(r.model, r.raw, r.len, &v) || v.empty) {
            continue;
        }
        const char *st = ch_state_text(r.model, &v);
        occupied++;
        if (strcmp(st, "done") == 0) {
            done++;
        } else if (strcmp(st, "error") == 0) {
            *error = true;
        }
    }
    *all_done = occupied && done == occupied;
}

static bool storage_full(void)
{
    store_stats_t s;
    store_stats(&s);
    if (g_cfg.flash_log && s.flash_ok) {
        return s.flash_used * 100 >= s.flash_sectors * 95 && s.fetched_seq < s.flash_first_seq + 1;
    }
    return s.ram_count == s.ram_cap && s.fetched_seq < s.ram_first_seq;
}

static void led_task(void *arg)
{
    uint32_t last_answers = 0, blink_until = 0, tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;
        uint32_t now = store_uptime10();
        ble_status_t b;
        net_status_t n;
        ble_get_status(&b);
        net_get_status(&n);
        bool bt_ok = strcmp(b.state, "connected") == 0 && b.last_answer_up10 && now - b.last_answer_up10 < 150;
        bool bt_err = !bt_ok && b.last_error[0];
        bool wifi_err = !n.sta_connected && now > 200;      // give it 20 s after boot
        bool all_done = false, slot_err = false;
        if (bt_ok) {
            slot_summary(b.src, &all_done, &slot_err);
        }
        if (b.answers != last_answers) {
            last_answers = b.answers;
            blink_until = tick + 2;                         // 100 ms dark
        }
        bool second = (tick / 10) % 2;                      // alternate every 0.5 s
        rgb_t a, c;
        float level = 1;
        if (ble_blocked()) {
            a = VIOLET;
            c = wifi_err ? ORANGE : VIOLET;
        } else if (slot_err || (bt_err && !bt_ok)) {
            a = RED;
            c = wifi_err ? ORANGE : RED;
        } else if (bt_ok) {
            bool full = storage_full();
            if (wifi_err) {             // blue / orange, green / orange (done), red / orange (full)
                a = full ? RED : all_done ? GREEN : BLUE;
                c = ORANGE;
            } else {                    // blue, blue / red (full), blue / green (done)
                a = BLUE;
                c = full ? RED : all_done ? GREEN : BLUE;
            }
        } else if (wifi_err) {
            a = c = ORANGE;
        } else {
            a = c = BLUE;                                   // searching: slow pulse
            level = 0.15f + 0.85f * (0.5f + 0.5f * sinf(tick * 0.08f));
        }
        rgb_t col = second ? c : a;
        if (bt_ok && tick < blink_until) {
            col = OFF;
        }
        show(col, level);
    }
}

void led_start(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = g_cfg.led_gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) {
        ESP_LOGW(TAG, "no LED on GPIO %d", g_cfg.led_gpio);
        return;
    }
    led_strip_clear(strip);
    xTaskCreate(led_task, "led", 3072, NULL, 2, NULL);
}
