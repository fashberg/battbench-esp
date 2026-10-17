// Settings (NVS namespace "bb") and the table of chargers seen so far.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FW_VERSION "2026.10.4"
#define MAX_CHARGERS 8          // records carry a 3-bit charger index ("src")

typedef struct {
    char ssid[33];
    char pass[65];
    char hostname[32];
    char ap_pass[65];           // own access point while no Wi-Fi is connected ("" = open)
    char ble_addr[18];          // charger to connect to, "AA:BB:CC:DD:EE:FF"; "" = first SkyRC charger found
    uint16_t poll_ms;           // one round over all slots
    uint16_t thin_secs;         // flash log: one reading per slot every n s, changes at once (0 = every reading)
    bool flash_log;             // write the flash log at all
    uint8_t led_gpio;           // WS2812 status LED (DevKitC-1: 48 on v1.0, 38 on v1.1)
    bool ble_on;                // Bluetooth switched on (off: the charger is free for the SkyRC app / BattBench)
} bb_config_t;

typedef struct {
    char addr[18];
    uint8_t model;              // ch_model_t
    uint32_t last_boot;         // boot counter when last connected (oldest entry is replaced)
} charger_entry_t;

extern bb_config_t g_cfg;
extern charger_entry_t g_chargers[MAX_CHARGERS];
extern uint16_t g_boot;         // boot counter, part of every record's time stamp

void config_init(void);          // nvs_flash_init, load settings, count the boot
esp_err_t config_save(void);
int charger_src(const char *addr, uint8_t model);   // index in g_chargers (added / refreshed and saved)
