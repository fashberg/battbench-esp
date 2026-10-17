#include "ble.h"

#include <stdio.h>
#include <string.h>
#include "charger.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store.h"

static const char *TAG = "ble";

#define BIT_SYNC (1 << 0)
#define BIT_CONNECTED (1 << 1)
#define BIT_CONN_FAIL (1 << 2)
#define BIT_DISCONNECTED (1 << 3)
#define BIT_OP_DONE (1 << 4)
#define BIT_SCAN_DONE (1 << 5)

typedef struct {
    uint8_t len;
    uint8_t d[244];             // up to the MTU (history pages are longer than the status answers)
} pkt_t;

// one command from the web server at a time (ble_command), carried out by the poll loop
static SemaphoreHandle_t cmd_lock, cmd_done;
static struct {
    const uint8_t *req;
    int len, max, timeout_ms, result;
    uint8_t *reply;
    volatile bool pending;
} cmd;

static EventGroupHandle_t ev;
static QueueHandle_t inbox;
static uint8_t own_addr_type;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool scanning, want_reconnect, want_scan;
static volatile uint32_t pause_until;           // uptime 0.1 s; Bluetooth paused until then
static volatile int op_status;
static uint16_t svc_start, svc_end, val_handle, cccd_handle;

static ble_status_t st = {.state = "starting", .src = -1};
static ble_seen_t seen[BLE_SEEN_MAX];
static ble_addr_t seen_addr[BLE_SEEN_MAX];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void fmt_addr(const ble_addr_t *a, char *out)
{
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static void set_error(const char *fmt, int code)
{
    snprintf(st.last_error, sizeof st.last_error, fmt, code);
    ESP_LOGW(TAG, "%s", st.last_error);
}

static void set_msg(const char *msg)
{
    strlcpy(st.last_error, msg, sizeof st.last_error);
    ESP_LOGW(TAG, "%s", msg);
}

// ------------------------------------------------------------------ GAP events (NimBLE host task)
static void note_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    char name[32] = "";
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 && f.name_len) {
        int n = f.name_len < sizeof name - 1 ? f.name_len : sizeof name - 1;
        memcpy(name, f.name, n);
        name[n] = 0;
    }
    char addr[18];
    fmt_addr(&d->addr, addr);
    bool generic = false;
    ch_model_t m = name[0] ? ch_from_name(name, &generic) : CH_UNKNOWN;
    taskENTER_CRITICAL(&mux);
    int idx = -1, oldest = 0;
    for (int i = 0; i < BLE_SEEN_MAX; i++) {
        if (strcmp(seen[i].addr, addr) == 0) {
            idx = i;
            break;
        }
        if (seen[i].up10 < seen[oldest].up10) {
            oldest = i;
        }
    }
    bool known = idx >= 0;
    if (!known && (m != CH_UNKNOWN || generic || strcmp(addr, g_cfg.ble_addr) == 0)) {
        idx = oldest;           // only SkyRC chargers (and the configured address) are listed
        memset(&seen[idx], 0, sizeof seen[idx]);
        strcpy(seen[idx].addr, addr);
    }
    if (idx >= 0) {
        seen_addr[idx] = d->addr;
        seen[idx].rssi = d->rssi;
        seen[idx].up10 = store_uptime10();
        if (name[0]) {          // the name may come in the scan response only
            strlcpy(seen[idx].name, name, sizeof seen[idx].name);
            seen[idx].model = m;
            seen[idx].generic = generic;
        }
    }
    taskEXIT_CRITICAL(&mux);
}

// Connection interval while polling (units of 1.25 ms): 100-150 ms. Wi-Fi shares the radio with Bluetooth; with the
// short interval chargers ask for, Bluetooth takes most of the air time and HTTP ran at 15-100 KB/s (~290 KB/s
// without Bluetooth). A status query every half second needs no short interval.
#define ITVL_MIN 80
#define ITVL_MAX 120
static volatile bool slow_itvl;         // set after the discovery, which is faster with the charger's interval

static void note_interval(void)
{
    struct ble_gap_conn_desc d;
    if (conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &d) == 0) {
        st.itvl_ms = d.conn_itvl * 5 / 4;
    }
}

static void slow_interval(void)
{
    slow_itvl = true;
    note_interval();
    struct ble_gap_upd_params p = {.itvl_min = ITVL_MIN, .itvl_max = ITVL_MAX, .latency = 0,
                                   .supervision_timeout = 600};   // 6 s
    int rc = ble_gap_update_params(conn, &p);
    if (rc) {
        ESP_LOGW(TAG, "connection interval: error %d", rc);
    }
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_DISC:
        note_adv(&e->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        scanning = false;
        xEventGroupSetBits(ev, BIT_SCAN_DONE);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            conn = e->connect.conn_handle;
            slow_itvl = false;
            note_interval();
            xEventGroupSetBits(ev, BIT_CONNECTED);
        } else {
            set_error("connect failed (%d)", e->connect.status);
            xEventGroupSetBits(ev, BIT_CONN_FAIL);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected, reason 0x%x", e->disconnect.reason);
        conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(ev, BIT_DISCONNECTED | BIT_CONN_FAIL | BIT_OP_DONE);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        pkt_t p;
        uint16_t len = OS_MBUF_PKTLEN(e->notify_rx.om);
        if (len > sizeof p.d) {
            len = sizeof p.d;
        }
        uint16_t out = 0;
        ble_hs_mbuf_to_flat(e->notify_rx.om, p.d, len, &out);
        p.len = out;
        xQueueSend(inbox, &p, 0);
        break;
    }
    case BLE_GAP_EVENT_MTU:
        st.mtu = e->mtu.value;
        break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        // the charger asks for a short interval: keep a long one once polling runs (see slow_interval)
        if (slow_itvl && e->conn_update_req.self_params->itvl_max < ITVL_MIN) {
            e->conn_update_req.self_params->itvl_min = ITVL_MIN;
            e->conn_update_req.self_params->itvl_max = ITVL_MAX;
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        note_interval();
        break;
    default:
        break;
    }
    return 0;
}

// ------------------------------------------------------------------ GATT client callbacks
static void op_done(int status)
{
    op_status = status;
    xEventGroupSetBits(ev, BIT_OP_DONE);
}

static int on_mtu(uint16_t c, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    st.mtu = mtu;
    op_done(err->status);
    return 0;
}

static int on_svc(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        svc_start = svc->start_handle;
        svc_end = svc->end_handle;
    } else {
        op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    }
    return 0;
}

static int on_chr(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        val_handle = chr->val_handle;
    } else {
        op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    }
    return 0;
}

static int on_dsc(uint16_t c, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    if (err->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0 && !cccd_handle) {
            cccd_handle = dsc->handle;
        }
    } else {
        op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    }
    return 0;
}

static int on_write(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    op_done(err->status);
    return 0;
}

// Starts a GATT procedure (rc = its return code) and waits until its callback says it is done.
static bool wait_op(int rc, const char *what)
{
    if (rc != 0) {
        snprintf(st.last_error, sizeof st.last_error, "%s: error %d", what, rc);
        ESP_LOGW(TAG, "%s", st.last_error);
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(ev, BIT_OP_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(b & BIT_OP_DONE) || op_status != 0 || conn == BLE_HS_CONN_HANDLE_NONE) {
        snprintf(st.last_error, sizeof st.last_error, "%s: %s %d", what, b & BIT_OP_DONE ? "status" : "timeout",
                 op_status);
        ESP_LOGW(TAG, "%s", st.last_error);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ manager task
static void start_scan(int ms)
{
    if (scanning) {
        return;
    }
    struct ble_gap_disc_params p = {.passive = 0, .filter_duplicates = 0};
    xEventGroupClearBits(ev, BIT_SCAN_DONE);
    int rc = ble_gap_disc(own_addr_type, ms, &p, gap_event, NULL);
    scanning = rc == 0;
    if (rc) {
        ESP_LOGW(TAG, "scan failed: %d", rc);
        xEventGroupSetBits(ev, BIT_SCAN_DONE);
    }
}

// Sends the status query of one slot and waits for its answer.
static void log_pkt(const char *what, const uint8_t *d, int len)
{
    char hex[3 * 64 + 1];
    int n = 0;
    for (int i = 0; i < len && i < 64; i++) {
        n += sprintf(hex + n, "%02x ", d[i]);
    }
    hex[n] = 0;
    ESP_LOGI(TAG, "%s (%d): %s", what, len, hex);
}

// Notifications nobody asked for (greeting etc.): logged and dropped.
static void drain(void)
{
    pkt_t p;
    while (xQueueReceive(inbox, &p, 0) == pdTRUE) {
        log_pkt("unsolicited", p.d, p.len);
    }
}

static int ask(ch_model_t m, int slot, uint8_t *out, int timeout_ms)
{
    uint8_t req[20];
    int n = ch_request(m, slot, req);
    drain();
    if (ble_gattc_write_no_rsp_flat(conn, val_handle, req, n) != 0) {
        return 0;
    }
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    pkt_t p;
    while (conn != BLE_HS_CONN_HANDLE_NONE) {
        TickType_t now = xTaskGetTickCount();
        if (now >= end || xQueueReceive(inbox, &p, end - now) != pdTRUE) {
            return 0;
        }
        if (ch_matches(m, slot, p.d, p.len)) {
            memcpy(out, p.d, p.len);
            return p.len;
        }
        log_pkt("other answer", p.d, p.len);
    }
    return 0;
}

// The charger to connect to from the last scan: the configured address, else the SkyRC charger with the best signal.
static int pick_target(ble_addr_t *a, ble_seen_t *info)
{
    int best = -1;
    taskENTER_CRITICAL(&mux);
    uint32_t now = store_uptime10();
    for (int i = 0; i < BLE_SEEN_MAX; i++) {
        if (!seen[i].addr[0] || now - seen[i].up10 > 300) {
            continue;
        }
        if (g_cfg.ble_addr[0]) {
            if (strcasecmp(seen[i].addr, g_cfg.ble_addr) == 0) {
                best = i;
            }
        } else if ((seen[i].model || seen[i].generic) && (best < 0 || seen[i].rssi > seen[best].rssi)) {
            best = i;
        }
    }
    if (best >= 0) {
        *a = seen_addr[best];
        *info = seen[best];
    }
    taskEXIT_CRITICAL(&mux);
    return best;
}

static bool connect_charger(void)
{
    ble_addr_t a;
    ble_seen_t info;
    st.state = "scanning";
    start_scan(6000);
    xEventGroupWaitBits(ev, BIT_SCAN_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    if (pick_target(&a, &info) < 0) {
        snprintf(st.last_error, sizeof st.last_error, "%s",
                 g_cfg.ble_addr[0] ? "configured charger not found" : "no SkyRC charger found");
        return false;
    }
    st.state = "connecting";
    strcpy(st.addr, info.addr);
    strcpy(st.name, info.name);
    ESP_LOGI(TAG, "connecting to %s (%s)", info.addr, info.name);
    xEventGroupClearBits(ev, BIT_CONNECTED | BIT_CONN_FAIL | BIT_DISCONNECTED | BIT_OP_DONE);
    int rc = ble_gap_connect(own_addr_type, &a, 15000, NULL, gap_event, NULL);
    if (rc) {
        set_error("connect: error %d", rc);
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(ev, BIT_CONNECTED | BIT_CONN_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(b & BIT_CONNECTED)) {
        if (!(b & BIT_CONN_FAIL)) {
            ble_gap_conn_cancel();
            set_msg("connect: timeout");
        }
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));     // like BattBench: give the charger a moment
    svc_start = svc_end = val_handle = cccd_handle = 0;
    if (!wait_op(ble_gattc_exchange_mtu(conn, on_mtu, NULL), "MTU")) {
        // not fatal: notifications are then cut to 20 bytes
    }
    if (!wait_op(ble_gattc_disc_svc_by_uuid(conn, BLE_UUID16_DECLARE(0xFFE0), on_svc, NULL), "service FFE0") ||
        !svc_start) {
        set_msg("service FFE0 missing");
        return false;
    }
    if (!wait_op(ble_gattc_disc_chrs_by_uuid(conn, svc_start, svc_end, BLE_UUID16_DECLARE(0xFFE1), on_chr, NULL),
                 "characteristic FFE1") || !val_handle) {
        set_msg("characteristic FFE1 missing");
        return false;
    }
    if (val_handle < svc_end) {
        wait_op(ble_gattc_disc_all_dscs(conn, val_handle, svc_end, on_dsc, NULL), "descriptors");
    }
    if (!cccd_handle) {
        cccd_handle = val_handle + 1;   // usual place
    }
    ESP_LOGI(TAG, "FFE0 %u-%u, FFE1 value %u, CCCD %u, MTU %d", svc_start, svc_end, val_handle, cccd_handle, st.mtu);
    uint8_t on[2] = {1, 0};
    if (!wait_op(ble_gattc_write_flat(conn, cccd_handle, on, sizeof on, on_write, NULL), "notifications")) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    drain();
    slow_interval();
    if (info.model != CH_MC3000) {
        // MC5000: answers the status query only after the app's handshake 0x57 (device info); skyrc-mc-rs sends
        // just this in its read-only mode. Never the init sequence 74 / 65 / FE (resets running tasks).
        // As the SkyRC app: 57 00, the own Bluetooth address (least significant byte first), 10 x 00. skyrc-mc-rs
        // sends the phone address of its capture; with that the charger answers the status query too, but
        // not with its device info (firmware 2.02).
        uint8_t hello[21] = {0x0F, 0x13, 0x57, 0x00};
        ble_hs_id_copy_addr(own_addr_type, hello + 4, NULL);
        for (int i = 2; i < 20; i++) {
            hello[20] += hello[i];
        }
        log_pkt("handshake", hello, sizeof hello);
        ble_gattc_write_no_rsp_flat(conn, val_handle, hello, sizeof hello);
        vTaskDelay(pdMS_TO_TICKS(2000));
        drain();                            // logs the answer (device info: serial, firmware) if one comes
    }
    // model from the name, or the status query it answers
    ch_model_t m = info.model;
    uint8_t buf[64];
    if (m == CH_UNKNOWN) {
        if (ask(CH_MC5000, 0, buf, 2500)) {
            m = CH_MC5000;
        } else if (ask(CH_MC3000, 0, buf, 2500)) {
            m = CH_MC3000;
        }
    } else if (!ask(m, 0, buf, 2500)) {
        m = CH_UNKNOWN;
    }
    if (m == CH_UNKNOWN) {
        set_msg("no answer to the status query");
        return false;
    }
    st.model = m;
    st.src = charger_src(info.addr, m);
    st.connected_up10 = store_uptime10();
    st.last_error[0] = 0;
    st.state = "connected";
    ESP_LOGI(TAG, "%s %s connected, MTU %d, charger index %d", ch_model_name(m), info.addr, st.mtu, st.src);
    return true;
}

static void run_command(void)
{
    cmd.result = 0;
    drain();
    if (ble_gattc_write_no_rsp_flat(conn, val_handle, cmd.req, cmd.len) == 0) {
        log_pkt("command", cmd.req, cmd.len);
        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.timeout_ms);
        pkt_t p;
        while (conn != BLE_HS_CONN_HANDLE_NONE) {
            TickType_t now = xTaskGetTickCount();
            if (now >= end || xQueueReceive(inbox, &p, end - now) != pdTRUE) {
                break;
            }
            if (p.len >= 3 && p.d[0] == 0x0F && p.d[2] == cmd.req[2]) {
                log_pkt("command answer", p.d, p.len);
                int n = p.len < cmd.max - cmd.result ? p.len : cmd.max - cmd.result;
                memcpy(cmd.reply + cmd.result, p.d, n);
                cmd.result += n;
                // 98 (voltage curve): the charger sends its pages one after the other (0F len 98 mask pages page
                // ...); all of them go into the reply, one after the other
                if (cmd.req[2] == 0x98 && p.len >= 6 && p.d[5] < p.d[4] && cmd.result < cmd.max) {
                    continue;
                }
                break;
            }
            log_pkt("other answer", p.d, p.len);
        }
    } else {
        cmd.result = -1;
    }
    cmd.pending = false;
    xSemaphoreGive(cmd_done);
}

int ble_command(const uint8_t *req, int len, uint8_t *reply, int max, int timeout_ms)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || strcmp(st.state, "connected") != 0 ||
        xSemaphoreTake(cmd_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return -1;
    }
    xSemaphoreTake(cmd_done, 0);
    cmd.req = req;
    cmd.len = len;
    cmd.reply = reply;
    cmd.max = max;
    cmd.timeout_ms = timeout_ms;
    cmd.pending = true;
    int r = -1;
    // the poll loop takes it within one round (up to 4 queries of 1.5 s)
    if (xSemaphoreTake(cmd_done, pdMS_TO_TICKS(timeout_ms + 8000)) == pdTRUE) {
        r = cmd.result;
    } else {
        cmd.pending = false;
    }
    xSemaphoreGive(cmd_lock);
    return r;
}

static void poll_loop(void)
{
    ch_model_t m = st.model;
    uint8_t buf[64];
    while (conn != BLE_HS_CONN_HANDLE_NONE && !want_reconnect && !ble_blocked()) {
        TickType_t t0 = xTaskGetTickCount();
        if (want_scan) {
            want_scan = false;
            start_scan(5000);
        }
        for (int slot = 0; slot < CH_SLOTS && conn != BLE_HS_CONN_HANDLE_NONE; slot++) {
            if (cmd.pending) {
                run_command();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            int n = ask(m, slot, buf, 1500);
            if (n) {
                st.answers++;
                st.last_answer_up10 = store_uptime10();
                store_add(st.src, m, slot, buf, n);
            } else {
                st.misses++;
            }
            vTaskDelay(pdMS_TO_TICKS(100));     // the MC3000 app waits 100 ms between queries
        }
        TickType_t spent = xTaskGetTickCount() - t0, round = pdMS_TO_TICKS(g_cfg.poll_ms);
        vTaskDelay(spent + pdMS_TO_TICKS(200) < round ? round - spent : pdMS_TO_TICKS(200));
    }
}

static void manager(void *arg)
{
    xEventGroupWaitBits(ev, BIT_SYNC, pdFALSE, pdFALSE, portMAX_DELAY);
    while (1) {
        want_reconnect = false;
        if (ble_blocked()) {                // off or paused: no connection, the charger is free for others
            st.state = g_cfg.ble_on ? "paused" : "off";
            st.connected_up10 = 0;
            st.last_error[0] = 0;
            uint32_t now = store_uptime10();
            st.paused_secs = g_cfg.ble_on && pause_until > now ? (pause_until - now) / 10 : 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        st.paused_secs = 0;
        if (connect_charger()) {
            poll_loop();
        }
        if (conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            xEventGroupWaitBits(ev, BIT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
            conn = BLE_HS_CONN_HANDLE_NONE;
        }
        st.state = "waiting";
        st.connected_up10 = 0;
        for (int i = 0; i < 50 && !want_reconnect; i++) {
            if (want_scan) {
                want_scan = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ------------------------------------------------------------------ NimBLE host
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    xEventGroupSetBits(ev, BIT_SYNC);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    ev = xEventGroupCreate();
    inbox = xQueueCreate(16, sizeof(pkt_t));
    cmd_lock = xSemaphoreCreateMutex();
    cmd_done = xSemaphoreCreateBinary();
    if (nimble_port_init() != ESP_OK) {
        st.state = "off";
        snprintf(st.last_error, sizeof st.last_error, "Bluetooth init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    xTaskCreate(manager, "ble_mgr", 6144, NULL, 5, NULL);
}

void ble_reconnect(void)
{
    want_reconnect = true;
}

void ble_pause(uint32_t minutes)
{
    pause_until = minutes ? store_uptime10() + minutes * 600 : 0;
    want_reconnect = true;
}

bool ble_blocked(void)
{
    return !g_cfg.ble_on || (pause_until && store_uptime10() < pause_until);
}

void ble_scan_request(void)
{
    want_scan = true;
}

void ble_get_status(ble_status_t *s)
{
    *s = st;
}

int ble_get_seen(ble_seen_t *out, int max)
{
    int n = 0;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < BLE_SEEN_MAX && n < max; i++) {
        if (seen[i].addr[0]) {
            out[n++] = seen[i];
        }
    }
    taskEXIT_CRITICAL(&mux);
    return n;
}
