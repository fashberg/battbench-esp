// Readings: every reading in a compressed ring buffer in PSRAM (lost on power off), a thinned and compressed log in
// the "bblog" flash partition (kept). Each reading gets a sequence number that never goes back (also across
// reboots); BattBench asks for everything after the last number it has. Recent numbers come from RAM (every
// reading), older ones from flash (only the thinned readings: numbers have gaps there).
//
// Fetching deletes nothing: both are rings, the oldest readings go when they are full. The thinning picks at
// most one reading per slot every thin_secs (changes at once) and writes it to flash right away, not in blocks.
// It holds back the newest reading of each slot until it knows whether it is needed; store_flush() writes those
// before a restart. A power loss loses them (at most thin_secs per slot) and the RAM ring.
//
// Time stamps: boot counter + uptime in 0.1 s. Once the clock is known (SNTP or set by BattBench) the offset of
// that boot is kept in NVS, so readings taken before the clock was set get their real time as well.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "esp_err.h"

#define REC_RAW_MAX 28

typedef struct {
    uint32_t seq;
    uint32_t up10;              // uptime of that boot, 0.1 s
    uint16_t boot;
    uint8_t src;                // index in g_chargers
    uint8_t slot;
    uint8_t len;
    uint8_t model;              // ch_model_t (RAM only; flash records get it from g_chargers)
    uint8_t raw[REC_RAW_MAX];   // the charger's answer as received
} rec_t;

typedef struct {
    uint32_t logid;             // changes when the log is cleared
    uint32_t seq_next;
    uint32_t ram_cap, ram_count, ram_first_seq;   // ram_cap: estimated from the bytes per reading so far
    uint32_t ram_first_up10;
    uint32_t ram_bytes, ram_bytes_cap;  // compressed size of the readings in RAM, size of the ring
    bool ram_full;              // the oldest readings in RAM have been dropped
    uint32_t flash_sectors, flash_used, flash_first_seq;
    uint16_t flash_first_boot;  // oldest sector whose boot has a known clock (older ones cannot be dated)
    uint32_t flash_first_up10;
    uint32_t flash_timed;       // sectors from that one to the newest (for the range estimate)
    uint32_t flash_recs, flash_bytes;   // written since boot (average record size)
    bool flash_ok;
    bool clock_set;
    uint32_t fetched_seq;       // BattBench has the readings up to this number (also from before a restart)
    uint32_t ram_open;          // readings in RAM BattBench has not fetched yet
    uint32_t flash_open;        // flash sectors (of flash_used) with readings it has not fetched yet
    int32_t fetch_secs;         // since the last fetch in this boot, -1 = none yet
    double fetch_t;             // unix time of the last fetch (also from before a restart), 0 = unknown
} store_stats_t;

// Record coding, shared by the flash log, the RAM ring and the binary fetch (/api/records?fmt=bin):
//   tag:   0 F SSS LLL  (F = full packet, S = charger index, L = slot); 0x80-0xFF: no record (end, or a marker)
//   varint seq - previous seq, varint up10 - previous up10 (first record: relative to the base)
//   full:  length, packet
//   delta: (same length as the previous packet of this charger / slot) bit mask of the changed bytes (1 bit per
//          byte, LSB first), then the changed bytes
#define STORE_ENC_MAX (1 + 2 * 5 + 1 + REC_RAW_MAX)
typedef struct {
    uint32_t seq, up10;         // of the previous record (or the base)
    uint8_t len[MAX_CHARGERS * 8];
    uint8_t raw[MAX_CHARGERS * 8][REC_RAW_MAX];
} store_coder_t;
void store_coder_reset(store_coder_t *c, uint32_t seq, uint32_t up10);
// Appends r (seq and up10 not lower than the previous record's) to buf, returns the bytes written.
int store_encode(store_coder_t *c, const rec_t *r, uint8_t *buf);

// Binary fetch (/api/records?fmt=bin), little endian:
//   header:  "BBR1", u32 log id, u32 seq_next, u8 more, u32 number of records
//   segment: 0x80, u16 boot, u8 clock known, i64 off10 (see store_off10), u32 seq, u32 up10 (base of the coding);
//            then the records of that boot, coded as above
//   0xFF ends it. Unix time of a record = (off10 + up10) / 10 if the clock of its boot is known.
#define STORE_BIN_HDR 17
#define STORE_STREAM_MAX (20 + STORE_ENC_MAX)
typedef struct {
    store_coder_t c;
    bool open;
    uint16_t boot;
} store_stream_t;
int store_bin_header(uint8_t *buf, uint32_t logid, uint32_t seq_next, bool more, uint32_t count);
// Appends r (a segment first if needed) to buf (room for STORE_STREAM_MAX bytes), returns the bytes written.
int store_stream_put(store_stream_t *st, const rec_t *r, uint8_t *buf);

void store_init(void);
uint32_t store_uptime10(void);
void store_add(uint8_t src, uint8_t model, uint8_t slot, const uint8_t *raw, int len);
void store_tick(void);                    // once a second: writes thinned readings that are due
// Records with seq > since in order, at most max; *more: there are further ones.
int store_read(uint32_t since, int max, rec_t *out, bool *more);
bool store_live(uint8_t src, uint8_t slot, rec_t *out);
// Unix time of a record time stamp; false if the clock of that boot is unknown.
bool store_unix(uint16_t boot, uint32_t up10, double *t);
// Unix time in 0.1 s minus uptime in 0.1 s of that boot; false if its clock is unknown.
bool store_off10(uint16_t boot, int64_t *off10);
void store_clock_set(void);               // the system clock is now right (SNTP / BattBench)
bool store_clock_ok(void);
void store_stats(store_stats_t *s);
void store_note_fetched(uint32_t since);  // BattBench asked for the readings after since
// Readings BattBench has not fetched yet: their number (in flash estimated from the sectors: *exact false) and
// the time of the oldest one. Returns false if it has all.
bool store_open(uint32_t *count, bool *exact, uint16_t *boot, uint32_t *up10);
void store_flush(void);                   // before a restart: write what the thinning holds back
esp_err_t store_clear(void);
