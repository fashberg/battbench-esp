#include "store.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "charger.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "store";
static SemaphoreHandle_t lock;

#ifndef RAM_BYTES
#define RAM_BYTES (5 * 1024 * 1024)     // PSRAM ring, compressed: ~10 bytes per reading = ~3 days of 4 slots every 2 s
#endif
#define RAM_FALLBACK (8 * 4096)         // no PSRAM
#define SEQ_BLOCK 10000                 // sequence numbers reserved in NVS at a time

static uint32_t logid, seq_next, seq_reserved;

// last fetch by BattBench; kept in NVS every FETCH_SAVE and before a restart
#define FETCH_SAVE 6000                 // 0.1 s
typedef struct {
    uint32_t logid, seq;
    double t;
} fetched_t;
static fetched_t fetched;
static uint32_t fetched_up10, fetched_saved_up10;
static bool fetched_now, fetched_dirty;

// ------------------------------------------------------------------ clock
#define ANCHORS 32
typedef struct {
    uint16_t boot;
    int64_t off10;              // unix time in 0.1 s minus uptime in 0.1 s
} anchor_t;
static anchor_t anchors[ANCHORS];
static bool clock_ok;

uint32_t store_uptime10(void)
{
    return (uint32_t)(esp_timer_get_time() / 100000);
}

static void nvs_put_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("bb", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, data, len);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_put_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("bb", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void store_clock_set(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t off10 = (int64_t)tv.tv_sec * 10 + tv.tv_usec / 100000 - store_uptime10();
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = -1, oldest = 0;
    for (int i = 0; i < ANCHORS; i++) {
        if (anchors[i].boot == g_boot) {
            idx = i;
        }
        // boot numbers wrap after 65535: "oldest" = furthest behind the current one
        if ((uint16_t)(g_boot - anchors[i].boot) > (uint16_t)(g_boot - anchors[oldest].boot)) {
            oldest = i;
        }
    }
    bool save = idx < 0 || llabs(anchors[idx].off10 - off10) > 20;
    if (idx < 0) {
        idx = oldest;
    }
    anchors[idx].boot = g_boot;
    anchors[idx].off10 = off10;
    clock_ok = true;
    if (save) {
        nvs_put_blob("anchors", anchors, sizeof anchors);
    }
    xSemaphoreGive(lock);
}

bool store_clock_ok(void)
{
    return clock_ok;
}

bool store_off10(uint16_t boot, int64_t *off10)
{
    for (int i = 0; i < ANCHORS; i++) {
        if (anchors[i].boot == boot && anchors[i].off10) {
            *off10 = anchors[i].off10;
            return true;
        }
    }
    return false;
}

bool store_unix(uint16_t boot, uint32_t up10, double *t)
{
    int64_t off10;
    if (!store_off10(boot, &off10)) {
        return false;
    }
    *t = (off10 + up10) / 10.0;
    return true;
}

// ------------------------------------------------------------------ record coding (format in store.h)
static int put_varint(uint8_t *p, uint32_t v)
{
    int n = 0;
    while (v >= 0x80) {
        p[n++] = (v & 0x7F) | 0x80;
        v >>= 7;
    }
    p[n++] = v;
    return n;
}

static bool get_varint(const uint8_t *p, uint32_t *off, uint32_t end, uint32_t *v)
{
    uint32_t r = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (*off >= end) {
            return false;
        }
        uint8_t b = p[(*off)++];
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return true;
        }
    }
    return false;
}

void store_coder_reset(store_coder_t *c, uint32_t seq, uint32_t up10)
{
    c->seq = seq;
    c->up10 = up10;
    memset(c->len, 0, sizeof c->len);
}

// r coded after c's previous record, without changing c (see coder_note)
static int coder_put(const store_coder_t *c, const rec_t *r, uint8_t *buf)
{
    int k = (r->src % MAX_CHARGERS) * 8 + r->slot % 8;
    bool full = c->len[k] != r->len;
    int n = 1;
    n += put_varint(buf + n, r->seq - c->seq);
    n += put_varint(buf + n, r->up10 - c->up10);
    if (full) {
        buf[n++] = r->len;
        memcpy(buf + n, r->raw, r->len);
        n += r->len;
    } else {
        int mbytes = (r->len + 7) / 8;
        uint8_t *mask = buf + n;
        memset(mask, 0, mbytes);
        n += mbytes;
        for (int i = 0; i < r->len; i++) {
            if (r->raw[i] != c->raw[k][i]) {
                mask[i / 8] |= 1 << (i % 8);
                buf[n++] = r->raw[i];
            }
        }
    }
    buf[0] = (full ? 0x40 : 0) | (r->src & 7) << 3 | (r->slot & 7);
    return n;
}

static void coder_note(store_coder_t *c, const rec_t *r)
{
    int k = (r->src % MAX_CHARGERS) * 8 + r->slot % 8;
    c->seq = r->seq;
    c->up10 = r->up10;
    c->len[k] = r->len;
    memcpy(c->raw[k], r->raw, r->len);
}

int store_encode(store_coder_t *c, const rec_t *r, uint8_t *buf)
{
    int n = coder_put(c, r, buf);
    coder_note(c, r);
    return n;
}

static int put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = v >> (8 * i);
    }
    return n;
}

int store_bin_header(uint8_t *buf, uint32_t logid, uint32_t seq_next, bool more, uint32_t count)
{
    memcpy(buf, "BBR1", 4);
    put_le(buf + 4, logid, 4);
    put_le(buf + 8, seq_next, 4);
    buf[12] = more;
    put_le(buf + 13, count, 4);
    return STORE_BIN_HDR;
}

int store_stream_put(store_stream_t *st, const rec_t *r, uint8_t *buf)
{
    int n = 0;
    if (!st->open || st->boot != r->boot || r->seq <= st->c.seq || r->up10 < st->c.up10) {
        int64_t off10 = 0;
        bool known = store_off10(r->boot, &off10);
        buf[n++] = 0x80;
        n += put_le(buf + n, r->boot, 2);
        buf[n++] = known;
        n += put_le(buf + n, (uint64_t)off10, 8);
        n += put_le(buf + n, r->seq, 4);
        n += put_le(buf + n, r->up10, 4);
        store_coder_reset(&st->c, r->seq, r->up10);
        st->open = true;
        st->boot = r->boot;
    }
    return n + store_encode(&st->c, r, buf + n);
}

// Next record at *off (before end) into r (seq, up10, src, slot, len, raw); false at the end or on broken data.
static bool coder_get(store_coder_t *c, const uint8_t *p, uint32_t *off, uint32_t end, rec_t *r)
{
    uint32_t d1, d2;
    if (*off >= end || (p[*off] & 0x80)) {
        return false;
    }
    uint8_t tag = p[(*off)++];
    int src = (tag >> 3) & 7, slot = tag & 7, k = src * 8 + slot;
    if (!get_varint(p, off, end, &d1) || !get_varint(p, off, end, &d2)) {
        return false;
    }
    if (tag & 0x40) {
        if (*off >= end) {
            return false;
        }
        uint8_t len = p[(*off)++];
        if (len > REC_RAW_MAX || *off + len > end) {
            return false;
        }
        memcpy(c->raw[k], p + *off, len);
        c->len[k] = len;
        *off += len;
    } else {
        int len = c->len[k], mbytes = (len + 7) / 8;
        if (!len || *off + mbytes > end) {
            return false;
        }
        const uint8_t *mask = p + *off;
        *off += mbytes;
        for (int i = 0; i < len; i++) {
            if (mask[i / 8] & (1 << (i % 8))) {
                if (*off >= end) {
                    return false;
                }
                c->raw[k][i] = p[(*off)++];
            }
        }
    }
    c->seq += d1;
    c->up10 += d2;
    r->seq = c->seq;
    r->up10 = c->up10;
    r->src = src;
    r->slot = slot;
    r->len = c->len[k];
    memcpy(r->raw, c->raw[k], r->len);
    return true;
}

// ------------------------------------------------------------------ RAM ring
// Every reading of this boot, coded as above in blocks of RBLK bytes (each starts with a full packet per slot);
// when the ring is full the oldest block goes. Sequence numbers in RAM have no gaps.
#define RBLK 4096
typedef struct {
    uint32_t first_seq, up10_base;
    uint16_t used;              // bytes
} rblk_t;
static uint8_t *rdata;
static rblk_t *rblk;
static uint32_t rnblk, roldest, rused, rcur;      // rused: blocks in use from roldest on, rcur: the newest
static bool rfull;
static uint32_t rrecs, rbytes;                     // coded since the ring was last empty (bytes per reading)
static store_coder_t renc, rdec;
static rec_t live[MAX_CHARGERS][8];
static bool live_ok[MAX_CHARGERS][8];

static rblk_t *rblk_at(uint32_t i)
{
    return &rblk[(roldest + i) % rnblk];
}

static uint32_t ram_first(void)
{
    return rused ? rblk_at(0)->first_seq : seq_next;
}

static void ram_add(const rec_t *r)
{
    if (!rnblk) {
        return;
    }
    uint8_t buf[STORE_ENC_MAX];
    int n = rused ? coder_put(&renc, r, buf) : 0;
    if (!rused || rblk[rcur].used + n > RBLK) {
        if (rused == rnblk) {           // full: the oldest block goes
            roldest = (roldest + 1) % rnblk;
            rused--;
            rfull = true;
        }
        rcur = rused ? (rcur + 1) % rnblk : roldest;
        rblk[rcur] = (rblk_t){r->seq, r->up10, 0};
        rused++;
        store_coder_reset(&renc, r->seq, r->up10);
        n = coder_put(&renc, r, buf);
    }
    memcpy(rdata + rcur * RBLK + rblk[rcur].used, buf, n);
    rblk[rcur].used += n;
    coder_note(&renc, r);
    rrecs++;
    rbytes += n;
}

// Readings with seq > since in order, at most max.
static int ram_read(uint32_t since, rec_t *out, int max)
{
    if (!rused || since + 1 >= seq_next) {
        return 0;
    }
    uint32_t lo = 0, hi = rused;        // the last block that starts at or before since + 1
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rblk_at(mid)->first_seq <= since + 1) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    int n = 0;
    for (uint32_t i = lo; i < rused && n < max; i++) {
        const rblk_t *b = rblk_at(i);
        const uint8_t *p = rdata + ((roldest + i) % rnblk) * RBLK;
        uint32_t off = 0;
        store_coder_reset(&rdec, b->first_seq, b->up10_base);
        while (n < max && coder_get(&rdec, p, &off, b->used, &out[n])) {
            if (out[n].seq > since) {
                out[n].boot = g_boot;
                out[n].model = g_chargers[out[n].src].model;
                n++;
            }
        }
    }
    return n;
}

// ------------------------------------------------------------------ flash log
// Sector (4 KB): header, then records (coded as in store.h). A record is written body first and its tag byte
// last, so a record cut by a power loss leaves the tag erased (0xFF) and ends the sector. Every boot starts a new
// sector.
#define SECTOR 4096
#define MAGIC 0x314C4242u               // "BBL1"
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t logid;
    uint32_t sec_no;            // counts up, the oldest sector has the lowest number
    uint32_t first_seq;
    uint32_t up10_base;
    uint16_t boot;
    uint16_t rsv;
} sec_hdr_t;

typedef struct {
    uint32_t sec_no;            // 0 = not part of the log
    uint32_t first_seq;
    uint32_t up10_base;
    uint16_t boot;
} sec_info_t;

static const esp_partition_t *part;
static uint32_t nsec;
static sec_info_t *secs;
static uint32_t sec_no_last, oldest_pos, used;    // used: sectors of the log, from oldest_pos on
static bool cur_open;                              // a sector is being written (the newest one)
static uint32_t cur_pos, wpos;
static store_coder_t fenc, fdec;
static uint8_t *sbuf;                               // one sector for reading
static uint32_t flash_recs, flash_bytes;
static uint32_t last_written;                      // sequence number of the newest record in flash

static void flash_scan(void)
{
    sec_no_last = 0;
    used = 0;
    oldest_pos = 0;
    uint32_t newest_pos = 0, oldest_no = UINT32_MAX;
    sec_hdr_t h;
    for (uint32_t i = 0; i < nsec; i++) {
        memset(&secs[i], 0, sizeof secs[i]);
        if (esp_partition_read(part, i * SECTOR, &h, sizeof h) != ESP_OK || h.magic != MAGIC || h.sec_no == 0 ||
            h.sec_no == UINT32_MAX) {
            continue;
        }
        if (h.sec_no > sec_no_last) {
            sec_no_last = h.sec_no;     // numbers keep counting even over a cleared log
        }
        if (h.logid != logid) {
            continue;
        }
        secs[i] = (sec_info_t){h.sec_no, h.first_seq, h.up10_base, h.boot};
        if (h.sec_no < oldest_no) {
            oldest_no = h.sec_no;
            oldest_pos = i;
        }
        if (h.sec_no >= secs[newest_pos].sec_no) {
            newest_pos = i;
        }
    }
    if (oldest_no != UINT32_MAX) {
        used = (newest_pos + nsec - oldest_pos) % nsec + 1;
        cur_pos = newest_pos;           // the next sector goes after it
    } else {
        cur_pos = nsec - 1;
    }
    cur_open = false;
}

static bool flash_new_sector(const rec_t *r)
{
    uint32_t pos = (cur_pos + 1) % nsec;
    if (used == nsec) {                 // full: the oldest sector goes
        oldest_pos = (oldest_pos + 1) % nsec;
        used--;
    }
    if (used == 0) {
        oldest_pos = pos;
    }
    memset(&secs[pos], 0, sizeof secs[pos]);
    if (esp_partition_erase_range(part, pos * SECTOR, SECTOR) != ESP_OK) {
        ESP_LOGE(TAG, "erasing sector %lu failed", (unsigned long)pos);
        return false;
    }
    sec_hdr_t h = {MAGIC, logid, ++sec_no_last, r->seq, r->up10, r->boot, 0};
    if (esp_partition_write(part, pos * SECTOR, &h, sizeof h) != ESP_OK) {
        return false;
    }
    secs[pos] = (sec_info_t){h.sec_no, h.first_seq, h.up10_base, h.boot};
    used++;
    cur_pos = pos;
    cur_open = true;
    wpos = sizeof h;
    store_coder_reset(&fenc, r->seq, r->up10);
    return true;
}

static void flash_write(const rec_t *r)
{
    if (!part || !g_cfg.flash_log) {
        return;
    }
    uint8_t buf[STORE_ENC_MAX];
    int n = 0;
    if (r->seq <= last_written) {
        ESP_LOGW(TAG, "reading %lu out of order, not written", (unsigned long)r->seq);
        return;
    }
    if (cur_open && secs[cur_pos].boot == r->boot && r->seq > fenc.seq && r->up10 >= fenc.up10) {
        n = coder_put(&fenc, r, buf);
    }
    if (n == 0 || wpos + n > SECTOR) {
        if (!flash_new_sector(r)) {
            cur_open = false;
            return;
        }
        n = coder_put(&fenc, r, buf);
    }
    // body first, the tag byte last (see above)
    if (esp_partition_write(part, cur_pos * SECTOR + wpos + 1, buf + 1, n - 1) != ESP_OK ||
        esp_partition_write(part, cur_pos * SECTOR + wpos, buf, 1) != ESP_OK) {
        ESP_LOGE(TAG, "flash write failed");
        cur_open = false;
        return;
    }
    wpos += n;
    last_written = r->seq;
    flash_recs++;
    flash_bytes += n;
    coder_note(&fenc, r);
}

// Decodes sector pos into out (records with since < seq < limit), returns the count; *stop: limit reached.
static int flash_decode(uint32_t pos, uint32_t since, uint32_t limit, rec_t *out, int max, bool *stop)
{
    const sec_info_t *si = &secs[pos];
    if (!si->sec_no || esp_partition_read(part, pos * SECTOR, sbuf, SECTOR) != ESP_OK) {
        return 0;
    }
    uint32_t off = sizeof(sec_hdr_t);
    int n = 0;
    store_coder_reset(&fdec, si->first_seq, si->up10_base);
    while (n < max && coder_get(&fdec, sbuf, &off, SECTOR, &out[n])) {
        if (out[n].seq >= limit) {
            *stop = true;
            break;
        }
        if (out[n].seq > since) {
            out[n].boot = si->boot;
            out[n].model = g_chargers[out[n].src].model;
            n++;
        }
    }
    return n;
}

static int flash_read(uint32_t since, uint32_t limit, rec_t *out, int max)
{
    if (!part || !used) {
        return 0;
    }
    // last sector that starts at or before since + 1 (the sectors are in sequence order)
    uint32_t start = 0;
    for (uint32_t i = 0; i < used; i++) {
        const sec_info_t *si = &secs[(oldest_pos + i) % nsec];
        if (si->sec_no && si->first_seq <= since + 1) {
            start = i;
        } else if (si->sec_no && si->first_seq > since + 1) {
            break;
        }
    }
    int n = 0;
    bool stop = false;
    for (uint32_t i = start; i < used && n < max && !stop; i++) {
        n += flash_decode((oldest_pos + i) % nsec, since, limit, out + n, max - n, &stop);
    }
    return n;
}

// ------------------------------------------------------------------ thinning (as db.SampleThinner in BattBench)
// At most one reading per slot every thin_secs; a change of status / mode / error / type, current direction or
// empty / occupied is written at once together with the reading before it and the last one under load.
typedef struct {
    bool has_stored, has_pending, has_loaded;
    int8_t st_dir;
    uint32_t st_up10, st_state;
    rec_t pending, loaded;
} thin_t;
static thin_t *thin;                  // MAX_CHARGERS * 8

static int thin_keep(thin_t *th, const rec_t *r, rec_t *out)
{
    uint32_t secs10 = g_cfg.thin_secs * 10;
    ch_values_t v;
    if (secs10 == 0 || !ch_parse(r->model, r->raw, r->len, &v)) {
        out[0] = *r;
        return 1;
    }
    int d = v.dir;
    int sticky = (d || !th->has_stored) ? d : th->st_dir;     // a pause keeps the direction
    uint32_t state = ch_state(&v);
    bool changed = !th->has_stored || state != th->st_state || sticky != th->st_dir;
    if (!changed) {
        uint32_t age = r->up10 - th->st_up10;
        if (v.empty || age < secs10 || (d == 0 && sticky != 0 && age < 3 * secs10)) {
            th->has_pending = !v.empty;
            if (!v.empty) {
                th->pending = *r;
            }
            if (d) {
                th->loaded = *r;
                th->has_loaded = true;
            }
            return 0;
        }
    }
    int n = 0;
    if (changed) {
        if (th->has_loaded) {
            out[n++] = th->loaded;
        }
        if (th->has_pending && !(th->has_loaded && th->pending.seq == th->loaded.seq)) {
            out[n++] = th->pending;
        }
    }
    out[n++] = *r;
    th->has_pending = th->has_loaded = false;
    th->has_stored = true;
    th->st_up10 = r->up10;
    th->st_state = state;
    th->st_dir = sticky;
    return n;
}

// Readings chosen by the thinning come out late (the one before a change); they wait here, sorted by sequence
// number, until no thinning state holds back an older one, so the flash log stays in order. A held-back reading
// older than HOLD_MAX (slot not answering) is given up.
#define OUTBUF 256
#define HOLD_MAX 1200                   // 0.1 s
static rec_t *outbuf;
static int outn;

static uint32_t held_back_min(void)
{
    uint32_t now = store_uptime10(), min = UINT32_MAX;
    for (int i = 0; i < MAX_CHARGERS * 8; i++) {
        thin_t *th = &thin[i];
        if (th->has_pending && (th->pending.boot != g_boot || now - th->pending.up10 > HOLD_MAX)) {
            th->has_pending = false;
        }
        if (th->has_loaded && (th->loaded.boot != g_boot || now - th->loaded.up10 > HOLD_MAX)) {
            th->has_loaded = false;
        }
        if (th->has_pending && th->pending.seq < min) {
            min = th->pending.seq;
        }
        if (th->has_loaded && th->loaded.seq < min) {
            min = th->loaded.seq;
        }
    }
    return min;
}

static void outbuf_flush(bool all)
{
    uint32_t limit = all ? UINT32_MAX : held_back_min();
    int k = 0;
    while (k < outn && (outbuf[k].seq < limit || outn - k > OUTBUF - 4)) {
        flash_write(&outbuf[k]);
        k++;
    }
    if (k) {
        memmove(outbuf, outbuf + k, (outn - k) * sizeof(rec_t));
        outn -= k;
    }
}

static void outbuf_add(const rec_t *r)
{
    if (outn >= OUTBUF) {
        outbuf_flush(true);
    }
    int i = outn;
    while (i > 0 && outbuf[i - 1].seq > r->seq) {
        outbuf[i] = outbuf[i - 1];
        i--;
    }
    outbuf[i] = *r;
    outn++;
}

static void save_fetched(void)
{
    if (fetched_dirty) {
        nvs_put_blob("fetched", &fetched, sizeof fetched);
        fetched_dirty = false;
        fetched_saved_up10 = store_uptime10();
    }
}

// ------------------------------------------------------------------ API
void store_init(void)
{
    lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open("bb", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof anchors;
        if (nvs_get_blob(h, "anchors", anchors, &len) != ESP_OK) {
            memset(anchors, 0, sizeof anchors);
        }
        if (nvs_get_u32(h, "logid", &logid) != ESP_OK) {
            logid = esp_random() | 1;
            nvs_set_u32(h, "logid", logid);
        }
        len = sizeof fetched;
        if (nvs_get_blob(h, "fetched", &fetched, &len) != ESP_OK || fetched.logid != logid) {
            memset(&fetched, 0, sizeof fetched);
        }
        if (nvs_get_u32(h, "seqres", &seq_next) != ESP_OK) {
            seq_next = 1;
        }
        seq_reserved = seq_next + SEQ_BLOCK;
        nvs_set_u32(h, "seqres", seq_reserved);
        nvs_commit(h);
        nvs_close(h);
    }

    rnblk = RAM_BYTES / RBLK;
    rdata = heap_caps_malloc(rnblk * RBLK, MALLOC_CAP_SPIRAM);
    if (!rdata) {
        ESP_LOGW(TAG, "no PSRAM, small ring buffer only");
        rnblk = RAM_FALLBACK / RBLK;
        rdata = heap_caps_malloc(rnblk * RBLK, MALLOC_CAP_8BIT);
    }
    rblk = heap_caps_calloc(rnblk, sizeof(rblk_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rdata || !rblk) {
        rnblk = 0;
    }
    thin = heap_caps_calloc(MAX_CHARGERS * 8, sizeof(thin_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!thin) {
        thin = calloc(MAX_CHARGERS * 8, sizeof(thin_t));
    }
    outbuf = heap_caps_malloc(OUTBUF * sizeof(rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!outbuf) {
        outbuf = malloc(OUTBUF * sizeof(rec_t));
    }
    sbuf = malloc(SECTOR);

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "bblog");
    if (part) {
        nsec = part->size / SECTOR;
        secs = heap_caps_calloc(nsec, sizeof(sec_info_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int64_t t0 = esp_timer_get_time();
        flash_scan();
        ESP_LOGI(TAG, "flash log: %lu of %lu sectors used, scan %lld ms", (unsigned long)used, (unsigned long)nsec,
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGE(TAG, "partition bblog missing");
    }
    ESP_LOGI(TAG, "ring buffer %lu KB, log id %08lx, next seq %lu", (unsigned long)(rnblk * RBLK / 1024),
             (unsigned long)logid, (unsigned long)seq_next);
}

void store_add(uint8_t src, uint8_t model, uint8_t slot, const uint8_t *raw, int len)
{
    if (len > REC_RAW_MAX) {
        len = REC_RAW_MAX;
    }
    rec_t r = {.up10 = store_uptime10(), .boot = g_boot, .src = src, .slot = slot, .len = len, .model = model};
    memcpy(r.raw, raw, len);
    xSemaphoreTake(lock, portMAX_DELAY);
    r.seq = seq_next++;
    if (seq_next >= seq_reserved) {
        seq_reserved = seq_next + SEQ_BLOCK;
        nvs_put_u32("seqres", seq_reserved);
    }
    ram_add(&r);
    if (src < MAX_CHARGERS && slot < 8) {
        live[src][slot] = r;
        live_ok[src][slot] = true;
    }
    rec_t out[3];
    int n = thin_keep(&thin[(src % MAX_CHARGERS) * 8 + slot % 8], &r, out);
    for (int i = 0; i < n; i++) {
        outbuf_add(&out[i]);
    }
    outbuf_flush(false);
    xSemaphoreGive(lock);
}

void store_tick(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    outbuf_flush(false);
    if (fetched_dirty && store_uptime10() - fetched_saved_up10 >= FETCH_SAVE) {
        save_fetched();
    }
    xSemaphoreGive(lock);
}

void store_flush(void)
{
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    int n = 0;
    for (int i = 0; thin && i < MAX_CHARGERS * 8; i++) {
        thin_t *th = &thin[i];
        if (th->has_loaded) {
            outbuf_add(&th->loaded);
            n++;
        }
        if (th->has_pending && !(th->has_loaded && th->pending.seq == th->loaded.seq)) {
            outbuf_add(&th->pending);
            n++;
        }
        th->has_pending = th->has_loaded = false;
    }
    outbuf_flush(true);
    save_fetched();
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "flushed, %d held-back readings written", n);
}

void store_note_fetched(uint32_t since)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (since < seq_next) {             // else a number from another log
        struct timeval tv;
        gettimeofday(&tv, NULL);
        fetched_dirty |= since != fetched.seq;
        fetched = (fetched_t){logid, since, clock_ok ? tv.tv_sec + tv.tv_usec / 1e6 : 0};
        fetched_up10 = store_uptime10();
        fetched_now = true;
    }
    xSemaphoreGive(lock);
}

// Readings in RAM after since (RAM has every number from ram_first() on).
static uint32_t ram_after(uint32_t since)
{
    uint32_t from = since + 1 > ram_first() ? since + 1 : ram_first();
    return rused && from < seq_next ? seq_next - from : 0;
}

// Position (from the oldest) of the last sector that starts at or before seq.
static uint32_t sec_index(uint32_t seq)
{
    uint32_t k = 0;
    while (k + 1 < used && secs[(oldest_pos + k + 1) % nsec].first_seq <= seq) {
        k++;
    }
    return k;
}

bool store_open(uint32_t *count, bool *exact, uint16_t *boot, uint32_t *up10)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t rfirst = ram_first();
    rec_t r;
    bool ok = false;
    *count = ram_after(fetched.seq);
    *exact = true;
    if (fetched.seq + 1 < rfirst && flash_read(fetched.seq, rfirst, &r, 1) == 1) {
        ok = true;
        // flash part: the sectors from the one with that reading to the one where RAM begins (later flash
        // records are in RAM as well)
        double per_sector = flash_recs > 20 ? SECTOR * (double)flash_recs / flash_bytes : SECTOR / 10.0;
        *count += (uint32_t)((sec_index(rfirst - 1) - sec_index(r.seq) + 0.5) * per_sector);
        *exact = false;
    } else if (ram_read(fetched.seq, &r, 1) == 1) {
        ok = true;
    }
    if (ok) {
        *boot = r.boot;
        *up10 = r.up10;
    }
    xSemaphoreGive(lock);
    return ok;
}

int store_read(uint32_t since, int max, rec_t *out, bool *more)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t rfirst = ram_first();
    int n = 0;
    if (since + 1 < rfirst) {
        n = flash_read(since, rfirst, out, max);
        if (n) {
            since = out[n - 1].seq;
        }
        if (n == max) {
            *more = true;
            xSemaphoreGive(lock);
            return n;
        }
        if (since + 1 < rfirst) {
            since = rfirst - 1;      // the rest of the gap is lost (not in flash)
        }
    }
    n += ram_read(since, out + n, max - n);
    *more = ram_after(n ? out[n - 1].seq : since) > 0;
    xSemaphoreGive(lock);
    return n;
}

bool store_live(uint8_t src, uint8_t slot, rec_t *out)
{
    if (src >= MAX_CHARGERS || slot >= 8) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = live_ok[src][slot];
    if (ok) {
        *out = live[src][slot];
    }
    xSemaphoreGive(lock);
    return ok;
}

void store_stats(store_stats_t *s)
{
    memset(s, 0, sizeof *s);
    xSemaphoreTake(lock, portMAX_DELAY);
    s->logid = logid;
    s->seq_next = seq_next;
    s->ram_count = ram_after(0);
    s->ram_bytes = rused ? (rused - 1) * RBLK + rblk[rcur].used : 0;
    s->ram_bytes_cap = rnblk * RBLK;
    s->ram_full = rfull;
    // full: what it holds; else estimated from the bytes per reading so far (at first ~10)
    s->ram_cap = rfull ? s->ram_count : (uint32_t)((double)s->ram_bytes_cap * (rrecs > 100 ? (double)rrecs / rbytes : 0.1));
    if (rused) {
        s->ram_first_seq = rblk_at(0)->first_seq;
        s->ram_first_up10 = rblk_at(0)->up10_base;
    }
    s->flash_ok = part != NULL;
    s->flash_sectors = nsec;
    s->flash_used = used;
    if (used) {
        s->flash_first_seq = secs[oldest_pos].first_seq;
        double t;
        for (uint32_t i = 0; i < used; i++) {
            const sec_info_t *si = &secs[(oldest_pos + i) % nsec];
            if (si->sec_no && store_unix(si->boot, si->up10_base, &t)) {
                s->flash_first_boot = si->boot;
                s->flash_first_up10 = si->up10_base;
                s->flash_timed = used - i;
                break;
            }
        }
    }
    s->flash_recs = flash_recs;
    s->flash_bytes = flash_bytes;
    s->clock_set = clock_ok;
    s->fetched_seq = fetched.seq;
    s->ram_open = ram_after(fetched.seq);
    if (used && fetched.seq < secs[oldest_pos].first_seq) {
        s->flash_open = used;                       // nothing in flash fetched yet
    } else if (used) {
        s->flash_open = used - 1 - sec_index(fetched.seq);    // the sectors after the one with that reading
        if (!s->flash_open && fetched.seq + 1 < seq_next && s->ram_open) {
            s->flash_open = 1;                      // the newest sector has unfetched readings as well
        }
    }
    s->fetch_secs = fetched_now ? (int32_t)((store_uptime10() - fetched_up10) / 10) : -1;
    s->fetch_t = fetched.t;
    xSemaphoreGive(lock);
}

esp_err_t store_clear(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    logid = esp_random() | 1;           // sectors of the old log no longer count; no need to erase them now
    nvs_put_u32("logid", logid);
    for (uint32_t i = 0; i < nsec; i++) {
        secs[i].sec_no = 0;
    }
    used = 0;
    cur_open = false;
    rused = rrecs = rbytes = 0;
    rfull = false;
    outn = 0;
    memset(thin, 0, MAX_CHARGERS * 8 * sizeof(thin_t));
    flash_recs = flash_bytes = 0;
    memset(&fetched, 0, sizeof fetched);
    fetched_now = false;
    fetched_dirty = true;
    xSemaphoreGive(lock);
    ESP_LOGW(TAG, "log cleared, new id %08lx", (unsigned long)logid);
    return ESP_OK;
}
