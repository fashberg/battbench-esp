#include "charger.h"

#include <ctype.h>
#include <string.h>

static uint8_t checksum(const uint8_t *p, int len)
{
    uint32_t s = 0;
    for (int i = 0; i < len; i++) {
        s += p[i];
    }
    return s & 0xFF;
}

static int be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

const char *ch_model_name(ch_model_t m)
{
    return m == CH_MC5000 ? "MC5000" : m == CH_MC3000 ? "MC3000" : "?";
}

ch_model_t ch_from_name(const char *name, bool *generic)
{
    char n[40];
    int i = 0;
    for (; name[i] && i < (int)sizeof n - 1; i++) {
        n[i] = tolower((unsigned char)name[i]);
    }
    n[i] = 0;
    *generic = false;
    if (strstr(n, "mc3000")) {
        return CH_MC3000;
    }
    if (strstr(n, "mc5000")) {
        return CH_MC5000;
    }
    if (strncmp(n, "#charger", 8) == 0 || (strncmp(n, "charger", 7) == 0 && (strstr(n, "ff-") || strstr(n, "ff34")))) {
        *generic = true;
    }
    return CH_UNKNOWN;
}

int ch_request(ch_model_t m, int slot, uint8_t *buf)
{
    if (m == CH_MC5000) {
        uint8_t bit = 1 << slot;
        buf[0] = 0x0F;
        buf[1] = 0x03;
        buf[2] = 0x91;
        buf[3] = bit;
        buf[4] = (0x91 + bit) & 0xFF;
        return 5;
    }
    memset(buf, 0, 20);
    buf[0] = 0x0F;
    buf[1] = 0x55;
    buf[2] = slot;
    buf[19] = checksum(buf, 19);
    return 20;
}

bool ch_matches(ch_model_t m, int slot, const uint8_t *p, int len)
{
    if (m == CH_MC5000) {
        // 0F <length> 91 bit ... cs, length = bytes after it (0x15 in captures of older firmware, 0x16 seen on
        // FW with one more byte), cs = sum from 91 on. Notifications may be cut to 20 bytes (default MTU): then
        // no checksum to check.
        return len >= 18 && p[0] == 0x0F && p[2] == 0x91 && p[3] == (1 << slot) &&
               (len != p[1] + 2 || checksum(p + 2, len - 3) == p[len - 1]);
    }
    return len >= 19 && p[0] == 0x0F && p[1] == 0x55 && p[2] == slot && checksum(p, len - 1) == p[len - 1];
}

bool ch_parse(ch_model_t m, const uint8_t *p, int len, ch_values_t *v)
{
    memset(v, 0, sizeof *v);
    if (m == CH_MC5000) {
        if (len < 18) {
            return false;
        }
        v->ma = be16(p + 4);
        v->mv = be16(p + 6);
        v->temp_c = be16(p + 8) / 1000;
        v->mah = be16(p + 10);
        v->secs = (p[12] << 24) | (p[13] << 16) | (p[14] << 8) | p[15];
        v->res = be16(p + 16);
        v->cut = len < 21;
        if (len >= 22) {
            v->status = p[18];
            v->mode = p[19];
            v->err = p[20];
            v->btype = p[21];
        } else {
            v->status = v->ma ? 2 : 0;          // cut notification: charging when current flows
        }
        v->empty = v->status == 0 && v->mv == 0;
        v->dir = v->ma == 0 ? 0 : v->status == 3 ? -1 : 1;
    } else {
        if (len < 19) {
            return false;
        }
        v->btype = p[3];
        v->mode = p[4];
        v->status = p[6];
        v->secs = be16(p + 7);
        v->mv = be16(p + 9);
        v->ma = be16(p + 11);
        v->mah = be16(p + 13);
        v->temp_c = p[15];
        v->res = be16(p + 16);
        v->err = v->status >= 128 ? v->status : 0;
        v->empty = v->status == 0 && v->mv == 0;
        v->dir = v->ma == 0 ? 0 : v->status == 2 ? -1 : 1;
    }
    if (v->res == 0xFFFF || v->res == 1) {
        v->res = 0;
    }
    return true;
}

uint32_t ch_state(const ch_values_t *v)
{
    return (uint32_t)(v->status & 0xFF) | (v->mode & 0xFF) << 8 | (v->err & 0x7F) << 16 | (v->btype & 0x7F) << 24 |
           (v->empty ? 0x80000000u : 0);
}

static const char *const MC5000_TYPES[] = {"LiIon", "LiHV", "LiFe", "NiMH", "NiCd", "Eneloop", "NiZn", "RAM", "LTO",
                                           "NaIon"};
static const char *const MC3000_TYPES[] = {"LiIon", "LiFe", "LiHV", "NiMH", "NiCd", "NiZn", "Eneloop", "RAM", "LTO"};

const char *ch_type_name(ch_model_t m, int btype)
{
    if (m == CH_MC5000 && btype >= 0 && btype < 10) {
        return MC5000_TYPES[btype];
    }
    if (m == CH_MC3000 && btype >= 0 && btype < 9) {
        return MC3000_TYPES[btype];
    }
    return "";
}

static const char *const MC5000_ERRORS[] = {"", "input voltage too low", "input voltage too high", "contact lost",
                                            "capacity limit reached", "time limit reached",
                                            "internal temperature too high", "calibration failed",
                                            "internal resistance too high", "contact lost", "wrong battery type",
                                            "overload", "reversed polarity", "battery already full"};
static const char *const MC3000_ERRORS[] = {"input voltage too low", "input voltage too high", "ADC error 1",
                                            "ADC error 2", "contact lost", "check voltage", "capacity limit reached",
                                            "time limit reached", "charger too hot", "battery too hot",
                                            "short circuit", "reversed polarity", "internal resistance too high"};

const char *ch_error_text(ch_model_t m, const ch_values_t *v)
{
    if (m == CH_MC5000 && v->err > 0 && v->err < 14) {
        return MC5000_ERRORS[v->err];
    }
    if (m == CH_MC3000 && v->status >= 128 && v->status < 141) {
        return MC3000_ERRORS[v->status - 128];
    }
    return "";
}

const char *ch_state_text(ch_model_t m, const ch_values_t *v)
{
    if (m == CH_MC5000) {
        // a task stopped by the charger (e.g. capacity limit) reports status 0 with the error for a while
        if (v->status == 5 || v->status == 6 || v->err == 13) {
            return "done";
        }
        if (v->err) {
            return "error";
        }
    }
    if (v->status == 0) {
        return v->empty ? "empty" : "idle";
    }
    if (m == CH_MC5000) {
        return v->status == 3 ? "discharging" : v->status == 4 ? "pause" : "charging";   // 4: rest between phases
    }
    if (v->status == 4) {
        return "done";
    }
    if (v->status >= 128) {
        return "error";
    }
    return v->status == 2 ? "discharging" : v->status == 3 ? "pause" : "charging";
}
