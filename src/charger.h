// SkyRC MC5000 / MC3000 status protocol (Bluetooth LE, service FFE0, characteristic FFE1), read only.
// Same as device_skyrc.py in BattBench (protocol from kolinger/skyrc-mc3000 and rssdev10/skyrc-mc-rs).
//
// MC5000: 0F 03 91 <bit> cs (bit 01/02/04/08 = slot 1-4), cs = sum(91, bit)
//   -> 0F 15 91 bit, mA, mV, temp (1/1000 deg C), mAh (u16 BE), seconds (u32 BE), mOhm (u16 BE),
//      status, mode, error, battery type, cs
// MC3000: 0F 55 <slot 0-3> 00 ... (20 bytes), last byte = sum of the others
//   -> 0F 55 slot, type, mode, cycle count, status, seconds, mV, mA, mAh (u16 BE), temp, mOhm (u16 BE), ...
// Only these status queries are ever sent. Never the MC5000 app's init sequence 57 / 74 / 65 / FE (restarts
// running tasks, FE even stops them), start / stop (93) or settings (94, MC3000 11).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CH_SLOTS 4

typedef enum { CH_UNKNOWN = 0, CH_MC5000 = 1, CH_MC3000 = 2 } ch_model_t;

typedef struct {
    int mv, ma, mah, res, secs;
    int temp_c;                 // deg C
    int status, mode, err, btype;
    int dir;                    // 1 charging, -1 discharging, 0 no current
    bool empty;                 // nothing in the slot
    bool cut;                   // MC5000 notification cut to 20 bytes: no status / mode / error / type
} ch_values_t;

const char *ch_model_name(ch_model_t m);
// From the advertised name: CH_MC5000 / CH_MC3000, *generic = true for "#Charger…" / "Charger FF-…"
ch_model_t ch_from_name(const char *name, bool *generic);
int ch_request(ch_model_t m, int slot, uint8_t *buf);              // query for one slot, returns its length
bool ch_matches(ch_model_t m, int slot, const uint8_t *p, int len); // answer to that query
bool ch_parse(ch_model_t m, const uint8_t *p, int len, ch_values_t *v);
// Everything that marks a change for the thinning of the flash log (status, mode, error, battery type, empty)
uint32_t ch_state(const ch_values_t *v);
const char *ch_type_name(ch_model_t m, int btype);
const char *ch_state_text(ch_model_t m, const ch_values_t *v);
const char *ch_error_text(ch_model_t m, const ch_values_t *v);     // "" = no error (texts as in BattBench)
