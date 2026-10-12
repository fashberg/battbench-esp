// Bluetooth LE central (NimBLE): finds the SkyRC charger, connects, polls all slots with the status query and
// hands every answer to the store. Reconnects by itself.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BLE_SEEN_MAX 16

typedef struct {
    char addr[18];
    char name[32];
    int rssi;
    uint32_t up10;              // last seen (uptime)
    uint8_t model;              // ch_model_t from the name (0 = unknown)
    bool generic;               // generic SkyRC name ("#Charger…"), model found out by asking
} ble_seen_t;

typedef struct {
    const char *state;          // "starting", "scanning", "connecting", "connected", "waiting", "paused", "off"
    char addr[18];
    char name[32];
    uint8_t model;
    int src;                    // charger index of the records (-1 = none yet)
    int mtu;
    uint32_t connected_up10;    // since (uptime)
    uint32_t answers, misses;   // status queries since boot
    uint32_t last_answer_up10;
    char last_error[64];
    uint32_t paused_secs;       // "paused": seconds left
} ble_status_t;

void ble_start(void);
void ble_reconnect(void);       // settings changed: drop the connection and search again
// The charger accepts one Bluetooth connection only: free it for the SkyRC app or BattBench for a while
// (minutes > 0, 0 = end the pause). Switched off for good: g_cfg.ble_on = false, then ble_reconnect().
void ble_pause(uint32_t minutes);
bool ble_blocked(void);         // off or paused
// Sends one packet to the connected charger between two status queries and waits for the answer with the same
// command byte (0F len cmd ...). Returns its length (0: none within timeout_ms, -1: not connected / busy).
// For BattBench's charger commands (programs, start / stop, history); the web handler decides what may go out.
int ble_command(const uint8_t *req, int len, uint8_t *reply, int max, int timeout_ms);
void ble_scan_request(void);    // one scan for the list in the web page (also while connected)
void ble_get_status(ble_status_t *s);
int ble_get_seen(ble_seen_t *out, int max);
