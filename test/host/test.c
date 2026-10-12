// Host test of store.c: feeds MC5000-like packets (from N8 readings), reboots midway, reads everything back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "charger.h"
#include "config.h"
#include "esp_partition.h"
#include "nvs.h"
#include "store.h"

int64_t fake_us;
bb_config_t g_cfg;
charger_entry_t g_chargers[MAX_CHARGERS];
uint16_t g_boot;

// ---- flash with NOR semantics (writes only clear bits)
static uint8_t *flash;
static esp_partition_t P;
static long bad_writes;
const esp_partition_t *esp_partition_find_first(int t, int s, const char *l) { return &P; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    if (off + len > P.size) return ESP_FAIL;
    memcpy(dst, flash + off, len);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    if (off + len > P.size) { printf("write past end\n"); return ESP_FAIL; }
    const uint8_t *s = src;
    for (size_t i = 0; i < len; i++) {
        if ((flash[off + i] & s[i]) != s[i]) bad_writes++;
        flash[off + i] &= s[i];
    }
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    if (off % 4096 || len % 4096 || off + len > P.size) { printf("bad erase\n"); return ESP_FAIL; }
    memset(flash + off, 0xFF, len);
    return ESP_OK;
}

// ---- NVS
static struct { char k[16]; uint8_t v[1024]; size_t len; } kv[16];
static int kv_find(const char *k, int add)
{
    for (int i = 0; i < 16; i++) if (strcmp(kv[i].k, k) == 0) return i;
    if (!add) return -1;
    for (int i = 0; i < 16; i++) if (!kv[i].k[0]) { strcpy(kv[i].k, k); return i; }
    return -1;
}
esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{
    int i = kv_find(k, 0);
    if (i < 0) return ESP_FAIL;
    memcpy(v, kv[i].v, 4);
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{
    int i = kv_find(k, 1);
    memcpy(kv[i].v, &v, 4);
    kv[i].len = 4;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{
    int i = kv_find(k, 0);
    if (i < 0 || kv[i].len > *len) return ESP_FAIL;
    memcpy(v, kv[i].v, kv[i].len);
    *len = kv[i].len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    int i = kv_find(k, 1);
    memcpy(kv[i].v, v, len);
    kv[i].len = len;
    return ESP_OK;
}

// ---- test
typedef struct { uint8_t slot, len, boot; uint8_t raw[23]; double t; } orig_t;
static orig_t *orig;              // by seq

int main(int argc, char **argv)
{
    int nsec = argc > 1 ? atoi(argv[1]) : 2528;
    g_cfg.thin_secs = argc > 2 ? atoi(argv[2]) : 10;
    bool soft = argc > 3 && strcmp(argv[3], "soft") == 0;   // reboot by esp_restart() instead of a power loss
    g_cfg.flash_log = true;
    g_cfg.poll_ms = 2000;
    P.size = nsec * 4096;
    flash = malloc(P.size);
    memset(flash, 0xFF, P.size);
    orig = calloc(400000, sizeof *orig);
    g_boot = 1;
    store_init();
    strcpy(g_chargers[0].addr, "AA:BB:CC:DD:EE:FF");
    g_chargers[0].model = CH_MC5000;

    FILE *f = fopen("n8.csv", "r");
    if (!f) {
        printf("n8.csv missing (make_csv.py)\n");
        return 2;
    }
    double t, t0 = -1, boot_t0 = 0;
    int slot, st, mv, ma, mah, res, temp, secs, n = 0, total = 21600;
    // count lines
    long lines = 0;
    char line[200];
    while (fgets(line, sizeof line, f)) lines++;
    rewind(f);
    long reboot_at = lines * 6 / 10;
    while (fscanf(f, "%lf,%d,%d,%d,%d,%d,%d,%d,%d", &t, &slot, &st, &mv, &ma, &mah, &res, &temp, &secs) == 9) {
        if (t0 < 0) t0 = boot_t0 = t - 5;
        if (n == reboot_at) {
            if (soft) {
                store_flush();          // restart (settings, update): held-back readings go to flash
            }
            g_boot = 2;
            boot_t0 = t - 5;
            store_init();
            printf("-- reboot --\n");
        }
        fake_us = (int64_t)((t - boot_t0) * 1e6);
        uint8_t p[23] = {0x0F, 0x15, 0x91, 1 << slot, ma >> 8, ma, mv >> 8, mv, 0, 0, mah >> 8, mah,
                         secs >> 24, secs >> 16, secs >> 8, secs, res >> 8, res, st, 0, 0, 3, 0};
        int tm = temp * 1000;
        p[8] = tm >> 8;
        p[9] = tm;
        unsigned cs = 0;
        for (int i = 2; i < 22; i++) cs += p[i];
        p[22] = cs;
        store_add(0, CH_MC5000, slot, p, 23);
        store_tick();
        store_stats_t s;
        store_stats(&s);
        uint32_t seq = s.seq_next - 1;
        orig[seq % 400000] = (orig_t){slot, 23, g_boot, {0}, t};
        memcpy(orig[seq % 400000].raw, p, 23);
        n++;
    }
    (void)total;
    store_stats_t s;
    store_stats(&s);
    printf("input %d readings over %.2f days\n", n, (t - t0) / 86400);
    printf("flash: %u records, %u bytes, %.2f bytes/record, %u of %u sectors, bad writes %ld\n", s.flash_recs,
           s.flash_bytes, (double)s.flash_bytes / s.flash_recs, s.flash_used, s.flash_sectors, bad_writes);
    printf("RAM: %u of %u, first seq %u; flash first seq %u\n", s.ram_count, s.ram_cap, s.ram_first_seq,
           s.flash_first_seq);

    // not fetched yet: count (estimated in flash) against the real number
    uint32_t fetch_at[] = {0, s.flash_first_seq + 5000, s.ram_first_seq + 100, s.seq_next - 1};
    for (int i = 0; i < 4; i++) {
        static rec_t tmp[1000];
        long real = 0;
        uint32_t sn = fetch_at[i];
        bool mr = true;
        while (mr) {
            int k = store_read(sn, 1000, tmp, &mr);
            if (!k) break;
            real += k;
            sn = tmp[k - 1].seq;
        }
        store_note_fetched(fetch_at[i]);
        uint32_t cnt = 0, ou = 0;
        uint16_t ob = 0;
        bool ex = false, op = store_open(&cnt, &ex, &ob, &ou);
        printf("open after %u: %s %u (real %ld)%s\n", fetch_at[i], ex ? "exact" : "about", cnt, real, op ? "" : ", none");
    }

    // read back everything
    static rec_t out[1000];
    uint32_t since = 0, last = 0;
    long got = 0, from_flash = 0, mism = 0, order = 0, boot1 = 0;
    bool more = true;
    double first_t = 0;
    while (1) {
        int k = store_read(since, 1000, out, &more);
        for (int i = 0; i < k; i++) {
            rec_t *r = &out[i];
            if (r->seq <= last) { order++; printf("order: %u after %u (boot %u, i %d of %d, since %u)\n", r->seq, last, r->boot, i, k, since); }
            last = r->seq;
            orig_t *o = &orig[r->seq % 400000];
            if (r->slot != o->slot || r->len != 23 || memcmp(r->raw, o->raw, 23) || r->boot != o->boot) mism++;
            if (r->seq < s.ram_first_seq) from_flash++;
            if (r->boot == 1) boot1++;
            if (!got) first_t = o->t;
            got++;
        }
        if (!k) break;
        since = out[k - 1].seq;
        if (!more) break;
    }
    printf("read back %ld (from flash %ld, boot 1: %ld), wrong %ld, out of order %ld, reaches back %.2f days\n",
           got, from_flash, boot1, mism, order, (t - first_t) / 86400);
    return mism || order || bad_writes ? 1 : 0;
}
