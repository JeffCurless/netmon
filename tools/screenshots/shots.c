/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */

/*
 * shots.c — render NetMon screens on the host, for the README.
 *
 * Compiles the real apps/netmon.c (included below, so its statics are in
 * reach) against stubs for the few kernel calls it makes.  The display
 * ioctls draw into a framebuffer with the picoOS driver's own font and
 * primitives (pulled out of picoOS/src/drivers/display.c by make.sh), so
 * the images match the panel pixel for pixel.  Synthetic scan windows go
 * through netmon's real merge code: the lists, survey, watch list and
 * Pairs history are built exactly as on the device.
 *
 * Writes one binary PPM per screen into the directory given as argv[1];
 * make.sh turns them into PNGs.  Host only: never part of the firmware.
 */

#include "netmon.c"

#include <stdarg.h>

/* ---- display: framebuffer + the driver's own drawing code --------------- */
static uint8_t framebuffer[DISP_WIDTH * DISP_HEIGHT];
static uint8_t bg_color = COLOR_BLACK;
#define dirty_mark_row(y) ((void)(y))
#include "font.inc"       /* font8x8[][] from display.c */
#include "draw.inc"       /* draw_*_impl() from display.c */

static uint32_t g_ms = 1000u;   /* simulated 1 ms tick */

int dev_open(dev_id_t id) { (void)id; return 0; }

int dev_ioctl(dev_id_t id, uint32_t cmd, void *arg)
{
    if (id == DEV_TIMER) { *(uint32_t *)arg = g_ms; return 0; }
    if (id != DEV_DISPLAY) return 0;
    switch (cmd) {
    case IOCTL_DISP_CLEAR:  memset(framebuffer, bg_color, sizeof framebuffer); break;
    case IOCTL_DISP_SET_BG: bg_color = *(uint8_t *)arg; break;
    case IOCTL_DISP_DRAW_PIXEL: {
        disp_pixel_arg_t *a = arg; draw_pixel_impl(a->x, a->y, a->color); break; }
    case IOCTL_DISP_DRAW_LINE: {
        disp_line_arg_t *a = arg;
        draw_line_impl(a->x0, a->y0, a->x1, a->y1, a->color); break; }
    case IOCTL_DISP_DRAW_RECT: {
        disp_rect_arg_t *a = arg;
        draw_rect_impl(a->x, a->y, a->w, a->h, a->color, a->filled != 0u); break; }
    case IOCTL_DISP_DRAW_TEXT: {
        disp_text_arg_t *a = arg;
        draw_text_impl(a->x, a->y, a->str, a->color, a->bg, a->scale ? a->scale : 1u);
        break; }
    case IOCTL_DISP_GET_BTNS: *(uint8_t *)arg = 0u; break;
    }
    return 0;
}

/* ---- kernel stubs -------------------------------------------------------- */
void kmutex_init(kmutex_t *m)   { (void)m; }
void kmutex_lock(kmutex_t *m)   { (void)m; }
void kmutex_unlock(kmutex_t *m) { (void)m; }
void kmem_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *largest)
{
    *used = 40960u; *free_bytes = 24576u; *largest = 20480u;
}
int fs_open(const char *n, int m)                { (void)n; (void)m; return -1; }
int fs_read(int fd, uint8_t *b, uint32_t n)      { (void)fd; (void)b; (void)n; return -1; }
int fs_write(int fd, const uint8_t *b, uint32_t n) { (void)fd; (void)b; return (int)n; }
int fs_close(int fd)                             { (void)fd; return 0; }
int fs_delete(const char *n)                     { (void)n; return 0; }
int32_t syscall_dispatch(uint32_t num, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    (void)num; (void)a; (void)b; (void)c; (void)d; return 0;
}
pcb_t *task_find_process(uint32_t pid) { (void)pid; return NULL; }
tcb_t *task_create_thread(pcb_t *p, const char *n, void (*e)(void *), void *a,
                          uint8_t pr, uint32_t ss)
{
    (void)p; (void)n; (void)e; (void)a; (void)pr; (void)ss; return NULL;
}
int  wifi_scan_start(wifi_scan_cb_t cb, void *ctx) { (void)cb; (void)ctx; return 0; }
void wifi_scan_stop(void) {}
int  wifi_scan_wait(const wifi_scan_list_t **l) { (void)l; return -1; }
int  bt_scan_start(bt_scan_cb_t cb, void *ctx) { (void)cb; (void)ctx; return 0; }
void bt_scan_stop(void) {}
int  bt_scan_wait(const bt_scan_list_t **l) { (void)l; return -1; }
const char *bt_devclass_str(bt_devclass_t c)
{
    static const char *const s[] = { "Unknown", "Computer", "Phone", "Network",
        "Audio", "Peripheral", "Imaging", "Wearable", "Toy", "Health", "Other" };
    return (unsigned)c < 11u ? s[c] : "Unknown";
}

/* ---- helpers ------------------------------------------------------------- */
static uint32_t s_rng = 12345u;
static int noise(int span)      /* -span..+span */
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 16) % (uint32_t)(2 * span + 1)) - span;
}

/* First OUI the table has for `vendor`, `nth` one in. */
static void oui_of(const char *vendor, int nth, uint8_t *mac)
{
    for (int i = 0; i < oui_table_len; i++) {
        if (strcmp(oui_names[oui_table[i] & 0xFFu], vendor) != 0) continue;
        if (nth-- > 0) continue;
        uint32_t o = oui_table[i] >> 8;
        mac[0] = (uint8_t)(o >> 16); mac[1] = (uint8_t)(o >> 8); mac[2] = (uint8_t)o;
        return;
    }
    fprintf(stderr, "no OUI for %s\n", vendor);
    exit(1);
}

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%u %u\n255\n", DISP_WIDTH, DISP_HEIGHT);
    for (uint32_t i = 0; i < DISP_WIDTH * DISP_HEIGHT; i++) {
        uint8_t c = framebuffer[i];
        /* RGB332 -> RGB565 as the driver's flush does, then to 8 bits. */
        unsigned r3 = c >> 5, g3 = (c >> 2) & 7u, b2 = c & 3u;
        unsigned r5 = (r3 << 2) | (r3 >> 1), g6 = (g3 << 3) | g3, b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        uint8_t px[3] = { (uint8_t)((r5 << 3) | (r5 >> 2)), (uint8_t)((g6 << 2) | (g6 >> 4)),
                          (uint8_t)((b5 << 3) | (b5 >> 2)) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void shot(ui_state_t *st, screen_t s, const char *dir, const char *name)
{
    st->screen = s;
    render_screen(st);
    save(dir, name);
}

/* ---- WiFi scenario ------------------------------------------------------- */
typedef struct {
    const char *ssid; const char *vendor; int nth; uint8_t last;
    uint8_t ch; int rssi; uint8_t auth; uint8_t phy; int gone_after;
} wnet_t;

static const wnet_t s_wnets[] = {
    { "HomeNet",       "eero",     0, 0x11,  6, -47, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "HomeNet",       "eero",     0, 0x12,  1, -64, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "HomeNet",       "eero",     0, 0x13, 11, -71, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "HomeNet-IoT",   "eero",     0, 0x21,  6, -52, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "NETGEAR42",     "Netgear",  2, 0x42,  9, -69, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT | WIFI_PHY_HT40, 0 },
    { "Garage",        "TP-Link",  1, 0x07, 11, -68, WIFI_AUTH_WPA | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM, 0 },
    { "",              "Ubiquiti", 0, 0x99, 11, -79, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "CoffeeShop",    "Cisco",    3, 0x5C,  1, -76, 0, WIFI_PHY_OFDM | WIFI_PHY_HT, 30 },
    { "Smith Family",  "ASUS",     0, 0x3A,  3, -81, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
    { "DIRECT-7F-HP",  "Intel",    0, 0x7F,  6, -84, WIFI_AUTH_RSN | WIFI_AUTH_WEP,
      WIFI_PHY_OFDM | WIFI_PHY_HT, 0 },
};
#define N_WNETS ((int)(sizeof s_wnets / sizeof s_wnets[0]))

static void wifi_window(uint32_t win, int walk)
{
    static wifi_scan_list_t l;
    memset(&l, 0, sizeof l);
    l.seq = win;
    l.window_ms = 1000u;
    for (int i = 0; i < N_WNETS; i++) {
        const wnet_t *n = &s_wnets[i];
        if (n->gone_after && (int)win > n->gone_after) continue;
        wifi_scan_result_t *r = &l.items[l.count++];
        snprintf(r->ssid, sizeof r->ssid, "%s", n->ssid);
        oui_of(n->vendor, n->nth, r->bssid);
        r->bssid[3] = 0x4E; r->bssid[4] = 0xA0; r->bssid[5] = n->last;
        r->channel   = n->ch;
        r->rssi      = (int16_t)(n->rssi + noise(2) + (i == 0 ? walk : 0));
        r->auth_mode = n->auth;
        r->noise     = (int8_t)(-92 + noise(1));
        r->snr       = (int8_t)(r->rssi - r->noise);
        r->beacon_tu = 100u;
        r->max_rate  = (n->phy & WIFI_PHY_OFDM) ? 108u : 22u;
        r->phy       = n->phy;
        r->capability = 0x0431u;
    }
    merge_wifi_window(&l);
}

/* ---- Bluetooth scenario -------------------------------------------------- */
enum { D_PHONE, D_AIRPODS, D_LAPTOP, D_MOUSE, D_HEADPH, D_TV, D_THERMO, D_TILE,
       D_BEACON, D_FAR, N_DEV };

static bt_scan_result_t s_dev[N_DEV];

static void dev_base(bt_scan_result_t *d, bt_devtype_t type)
{
    memset(d, 0, sizeof *d);
    d->type          = type;
    d->tx_power      = BT_TX_POWER_UNKNOWN;
    d->flags         = BT_FLAGS_NONE;
    d->company_id    = BT_COMPANY_NONE;
    d->service_uuid  = BT_SERVICE_NONE;
    d->svc_data_uuid = BT_SERVICE_NONE;
    d->appearance    = BT_APPEARANCE_NONE;
    d->addr_type     = type == BT_DEVTYPE_BLE ? BT_ADDR_RANDOM : BT_ADDR_TYPE_NONE;
    d->adv_type      = type == BT_DEVTYPE_BLE ? BT_ADV_IND : BT_ADV_TYPE_NONE;
    d->pkt_count     = 8u;
}

static void mfr(bt_scan_result_t *d, uint16_t company, int len, ...)
{
    va_list ap;
    va_start(ap, len);
    d->company_id = company;
    d->mfr_data_len = (uint8_t)len;
    for (int i = 0; i < len; i++) d->mfr_data[i] = (uint8_t)va_arg(ap, int);
    va_end(ap);
}

/* Address, printed order (a0 first), stored LSB first. */
static void addr(bt_scan_result_t *d, uint8_t a0, uint8_t a1, uint8_t a2,
                 uint8_t a3, uint8_t a4, uint8_t a5)
{
    d->addr[5] = a0; d->addr[4] = a1; d->addr[3] = a2;
    d->addr[2] = a3; d->addr[1] = a4; d->addr[0] = a5;
}

static void bt_devices(void)
{
    bt_scan_result_t *d;

    d = &s_dev[D_PHONE];   dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0x5A, 0x31, 0xC2, 0x8E, 0x14, 0xB7);           /* RPA */
    mfr(d, 0x004C, 6, 0x10, 0x05, 0x1D, 0x18, 0xA2, 0x3B);  /* Nearby Info */
    d->flags = 0x1A; d->tx_power = 12;

    d = &s_dev[D_AIRPODS]; dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0x4B, 0x09, 0x77, 0xD3, 0x6A, 0x2C);
    mfr(d, 0x004C, 10, 0x07, 0x19, 0x01, 0x14, 0x20, 0x2B, 0x99, 0x8F, 0x01, 0x00);
    d->adv_type = BT_ADV_NONCONN_IND;

    d = &s_dev[D_LAPTOP];  dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0x1E, 0x8C, 0x40, 0x55, 0xF2, 0x03);
    mfr(d, 0x0006, 4, 0x01, 0x0F, 0x20, 0x22);              /* CDP: Win laptop */
    d->adv_type = BT_ADV_NONCONN_IND;

    d = &s_dev[D_MOUSE];   dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0xD4, 0x61, 0x2E, 0x09, 0xBB, 0x71);
    snprintf(d->name, sizeof d->name, "MX Master 3");
    d->appearance = 0x03C2;                                 /* HID mouse */
    d->service_uuid = 0x1812; d->flags = 0x05; d->tx_power = 4;
    mfr(d, 0x01DA, 2, 0x02, 0x10);

    d = &s_dev[D_HEADPH];  dev_base(d, BT_DEVTYPE_CLASSIC);
    oui_of("Sony", 20, &d->addr[3]);
    { uint8_t t = d->addr[3]; d->addr[3] = d->addr[5]; d->addr[5] = t; }
    d->addr[2] = 0x8A; d->addr[1] = 0x3C; d->addr[0] = 0x51;
    snprintf(d->name, sizeof d->name, "WH-1000XM5");
    d->class_of_device = 0x240404;                          /* audio, headphones */
    d->dev_class = BT_CLASS_AUDIO;

    d = &s_dev[D_TV];      dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0xC8, 0x12, 0x5B, 0x6F, 0x90, 0x3D);
    d->addr_type = BT_ADDR_PUBLIC;
    oui_of("Samsung", 4, &d->addr[3]);
    { uint8_t t = d->addr[3]; d->addr[3] = d->addr[5]; d->addr[5] = t; }
    snprintf(d->name, sizeof d->name, "Living Room TV");
    mfr(d, 0x0075, 4, 0x42, 0x04, 0x01, 0x20);

    d = &s_dev[D_THERMO];  dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0xA4, 0xC1, 0x38, 0x5E, 0x1F, 0x2A);
    d->addr_type = BT_ADDR_PUBLIC;
    snprintf(d->name, sizeof d->name, "ATC_5E1F2A");
    d->adv_type = BT_ADV_NONCONN_IND;
    d->svc_data_uuid = 0x181A; d->svc_data_len = 13;
    {   /* ATC1441: MAC, temp 0.1 C BE, humidity %, battery %, mV BE, count */
        const uint8_t sd[13] = { 0xA4, 0xC1, 0x38, 0x5E, 0x1F, 0x2A,
                                 0x00, 0xD7, 0x2E, 0x55, 0x0B, 0x9A, 0x11 };
        memcpy(d->svc_data, sd, sizeof sd);
    }

    d = &s_dev[D_TILE];    dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0xE6, 0x3F, 0x21, 0x9C, 0x47, 0x0D);
    d->service_uuid = 0xFEED;
    d->svc_data_uuid = 0xFEED; d->svc_data_len = 8;
    d->adv_type = BT_ADV_IND;

    d = &s_dev[D_BEACON];  dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0xF0, 0x1D, 0x7A, 0x22, 0x5E, 0x90);
    mfr(d, 0x004C, 23, 0x02, 0x15,
        0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
        0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
        0x00, 0x01, 0x00, 0x2A, 0xC5);
    d->adv_type = BT_ADV_NONCONN_IND;

    d = &s_dev[D_FAR];     dev_base(d, BT_DEVTYPE_BLE);
    addr(d, 0x72, 0x88, 0x14, 0xAB, 0x3E, 0x61);
    mfr(d, 0x0075, 3, 0x01, 0x00, 0x02);
}

/* Windows run per radio: enough to fill the 320-column graphs. */
#define WIFI_WINDOWS  330u
#define BT_WINDOWS    330u
#define T_WALK        (BT_WINDOWS - 95u)
#define T_DESK        (BT_WINDOWS - 60u)

/* RSSI of each device in window `w`, or 0 when it is not heard. */
static int bt_rssi(int dev, uint32_t w)
{
    /* Someone carrying a phone and AirPods walks in at window T_WALK and
     * moves around the room; a laptop and its mouse arrive together at
     * T_DESK.  Both late enough to be inside the Pairs history. */
    int walk = 0;
    if (w >= T_WALK) {
        int t = (int)((w - T_WALK) % 40u);
        walk = t < 20 ? -t : t - 40;          /* triangle 0..-20..0 */
    }
    switch (dev) {
    case D_PHONE:   return w >= T_WALK ? -52 + walk + noise(2) : 0;
    case D_AIRPODS: return w >= T_WALK ? -58 + walk + noise(2) : 0;
    case D_LAPTOP:  return w >= T_DESK && w % 3u ? -71 + noise(3) : 0;
    case D_MOUSE:   return w >= T_DESK ? -75 + noise(3) : 0;
    case D_TV:      return -63 + noise(3) + (w >= T_WALK ? walk / 4 : 0);
    case D_HEADPH:  return w % 10u < 3u ? -66 + noise(2) : 0;   /* inquiry */
    case D_THERMO:  return w % 4u ? -78 + noise(3) : 0;
    case D_TILE:    return w % 5u < 3u ? -84 + noise(2) : 0;
    case D_BEACON:  return -70 + noise(3);
    case D_FAR:     return w % 7u == 0u || w % 11u == 0u ? -94 + noise(2) : 0;
    }
    return 0;
}

static void bt_window(uint32_t w)
{
    static bt_scan_list_t l;
    memset(&l, 0, sizeof l);
    l.seq = w;
    l.window_ms = 1000u;
    for (int i = 0; i < N_DEV; i++) {
        int r = bt_rssi(i, w);
        if (r == 0) continue;
        l.items[l.count] = s_dev[i];
        l.items[l.count].rssi = (int8_t)r;
        l.count++;
    }
    merge_bt_window(&l);
}

static int slot_of(int dev)
{
    return bt_find(s_dev[dev].addr);
}

/* Per-device graph history, as the detail panel would have built it had it
 * been open the whole time; loaded into s_bt_rssi_history per shot. */
static int8_t s_dev_hist[N_DEV][DISP_WIDTH];
static int    s_dev_head = 0;

static void bt_open(ui_state_t *st, int dev)
{
    st->bt_detail_idx = slot_of(dev);
    st->detail_scroll = 0;
    memcpy(s_bt_rssi_history, s_dev_hist[dev], sizeof s_bt_rssi_history);
    s_bt_graph_head = s_dev_head;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    kmutex_init(&s_wifi_lock);
    uint8_t bg = COLOR_PANEL;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);

    ui_state_t st;
    memset(&st, 0, sizeof st);

    /* Menus. */
    shot(&st, SCR_HOME, out, "home");
    st.sel = 2;
    shot(&st, SCR_BT_MENU, out, "bt-menu");
    st.sel = 0;

    /* Watch list entries, added before the scans so they collect events. */
    {
        wifi_scan_result_t hn = { .ssid = "HomeNet" };
        wifi_scan_result_t cs = { .ssid = "CoffeeShop" };
        watch_toggle_wifi(&hn);
        watch_toggle_wifi(&cs);
    }

    /* WiFi: HomeNet's detail graph and channel 6 history fill up. */
    s_ap_key_set = false;
    for (uint32_t w = 1u; w <= WIFI_WINDOWS; w++) {
        int t = (int)(w % 50u);
        int walk = t < 25 ? -t / 2 : (t - 50) / 2;
        if (w == 2u) {           /* open HomeNet's detail panel, as X does */
            s_detail_key = s_wifi_cache[0];
            s_ap_key     = s_detail_key;
            s_ap_key_set = true;
        }
        wifi_window(w, walk);
        g_ms += 1000u;
        s_rssi_history[s_graph_head] = s_wifi_seen[0] ? (int8_t)s_wifi_cache[0].rssi : 0;
        s_graph_head = (s_graph_head + 1) % (int)DISP_WIDTH;
        chan_hist_push(5);
    }
    st.sel = 0; st.scroll = 0;
    shot(&st, SCR_WIFI, out, "wifi-list");
    st.detail_idx = 0; st.detail_scroll = 0;
    shot(&st, SCR_WIFI_DETAIL, out, "wifi-detail");
    st.sel = 0; st.scroll = 0;
    shot(&st, SCR_WIFI_APS, out, "wifi-aps");
    st.sel = 5; st.chan_hist = false;
    shot(&st, SCR_CHANNELS, out, "channels");
    st.chan_hist = true;
    shot(&st, SCR_CHANNELS, out, "channels-history");

    /* Bluetooth, with a BT watch entry for the AirPods. */
    bt_devices();
    {
        bt_scan_result_t a = s_dev[D_AIRPODS];
        snprintf(a.name, sizeof a.name, "AirPods");
        watch_toggle_bt(&a);
        /* The watch entry keeps the label it was added with. */
    }
    for (uint32_t w = 1u; w <= BT_WINDOWS; w++) {
        bt_window(w);
        g_ms += 1000u;
        for (int i = 0; i < N_DEV; i++) {
            int a = slot_of(i);
            s_dev_hist[i][s_dev_head] = a >= 0 && s_bt_seen[a] ? s_bt_cache[a].rssi : 0;
        }
        s_dev_head = (s_dev_head + 1) % (int)DISP_WIDTH;
    }
    st.bt_filter = false; st.sel = 0; st.scroll = 0;
    shot(&st, SCR_BT, out, "bt-list");
    st.bt_filter = true;
    shot(&st, SCR_BT, out, "beacons");
    st.bt_filter = false;

    bt_open(&st, D_HEADPH);
    shot(&st, SCR_BT_DETAIL, out, "bt-detail");
    bt_open(&st, D_THERMO);
    shot(&st, SCR_BT_DETAIL, out, "bt-detail-sensor");
    bt_open(&st, D_TV);
    shot(&st, SCR_BT_FINDER, out, "bt-finder");

    corr_build();
    st.sel = 0; st.scroll = 0;
    shot(&st, SCR_BT_PAIRS, out, "pairs");
    if (s_pair_count > 0) {
        memcpy(st.pair_addr[0], s_bt_cache[s_pairs[0].a].addr, BT_ADDR_LEN);
        memcpy(st.pair_addr[1], s_bt_cache[s_pairs[0].b].addr, BT_ADDR_LEN);
        shot(&st, SCR_BT_PAIR, out, "pair-detail");
    }

    st.sel = 0; st.scroll = 0;
    shot(&st, SCR_WATCH, out, "watch");
    shot(&st, SCR_ABOUT, out, "about");
    return 0;
}
