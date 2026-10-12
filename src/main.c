// BattBench-ESP: reads a SkyRC MC5000 / MC3000 over Bluetooth LE, keeps the readings (RAM ring buffer + flash
// log) and serves them to BattBench over Wi-Fi. Web page for Wi-Fi / Bluetooth settings and status.
#include "ble.h"
#include "config.h"
#include "esp_log.h"
#include "led.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "store.h"
#include "web.h"

void app_main(void)
{
    config_init();
    store_init();
    esp_register_shutdown_handler(store_flush);     // every esp_restart(): settings, update, restart button
    net_start();
    web_start();
    ble_start();
    led_start();
    esp_ota_mark_app_valid_cancel_rollback();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        store_tick();
    }
}
