// Wi-Fi: station with the configured network; own access point "BattBench-XXXX" (192.168.4.1) while no network
// is connected. SNTP for the clock, mDNS <hostname>.local with the services _http._tcp and _battbench._tcp.
#pragma once
#include <stdbool.h>
#include "esp_wifi_types.h"

typedef struct {
    bool sta_connected;
    char ip[16];
    int rssi;
    bool ap_on;
    char ap_ssid[33];
    unsigned retries;
} net_status_t;

void net_start(void);
void net_get_status(net_status_t *s);
int net_scan(wifi_ap_record_t *out, int max);   // blocks ~3 s
