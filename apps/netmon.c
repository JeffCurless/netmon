/*
 * netmon.c — NetMon display UI application
 *
 * Screens: Home → Bluetooth (live scan) | WiFi (live scan) | About
 * Buttons: A=up  B=down  X=select  Y=back
 */

#include "kernel/syscall.h"
#include "kernel/dev.h"
#include "kernel/mem.h"
#include "drivers/display.h"
#include "netmon.h"
#include "kernel/wifi.h"
#include "kernel/bluetooth.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef NETMON_VERSION
#define NETMON_VERSION "?.?.?"
#endif
#ifndef PICOOS_VERSION_MAJOR
#define PICOOS_VERSION_MAJOR 0
#define PICOOS_VERSION_MINOR 0
#define PICOOS_VERSION_EDIT  0
#endif

/* ---- app color palette ---------------------------------------------------- *
 * Colors not in display.h (which has BLACK, WHITE, RED, GREEN, BLUE, YELLOW). */
#define COLOR_PANEL  RGB332(  0,   0,   0)   /* black    — content background   */
#define COLOR_HEADER RGB332(  0,   0, 128)   /* dark blue — title bar           */
#define COLOR_GREY   RGB332(190, 190, 190)   /* grey     — labels & static text */
#define COLOR_ORANGE RGB332(255, 165,   0)   /* orange   — limited WiFi signal  */

/* ---- scan timing ---------------------------------------------------------- */
#define RESCAN_DELAY_TICKS     40u  /* 40 × 50 ms = 2 s between WiFi scan cycles */
#define BT_RESCAN_DELAY_TICKS 120u  /* 120 × 50 ms = 6 s pause after BT scan completes */

/* ---- graph geometry ------------------------------------------------------- */
#define GRAPH_H     100u
#define GRAPH_Y_BOT ((uint16_t)(DISP_HEIGHT - 1u))
#define GRAPH_Y_TOP ((uint16_t)(DISP_HEIGHT - GRAPH_H))
#define GRAPH_GAP_W   8u          /* blank gap starting at the write head — marks current position */

/* ---- layout --------------------------------------------------------------- */
#define TITLE_H      20u
#define ROW_H        20u
#define SEL_X         4u
#define TEXT_X       18u
#define TRI_H        13u
#define TRI_W         9u
#define VISIBLE_ROWS  ((DISP_HEIGHT - TITLE_H) / ROW_H)

/* ---- state ---------------------------------------------------------------- */
typedef enum { SCR_HOME = 0, SCR_WIFI, SCR_BT, SCR_BT_DETAIL, SCR_ABOUT, SCR_WIFI_DETAIL } screen_t;

typedef struct {
    screen_t screen;
    int      sel;
    int      scroll;
    int      prev_sel;      /* home cursor saved when entering any sub-screen */
    int      prev_scroll;
    int      wifi_sel;      /* WiFi list cursor saved when entering detail panel */
    int      wifi_scroll;
    int      detail_idx;    /* s_wifi_cache index shown in the detail panel */
    int      rescan_ticks;  /* countdown to next WiFi scan trigger (50 ms ticks) */
    int      bt_sel;        /* BT list cursor saved when entering detail panel */
    int      bt_scroll;
    int      bt_detail_idx; /* s_bt_cache index shown in the BT detail panel */
    int      bt_rescan_ticks;
    bool     dirty;
    bool     scan_pending;
    bool     bt_scan_pending;
} ui_state_t;

/* ---- WiFi result cache ---------------------------------------------------- */

static wifi_scan_result_t s_wifi_cache[WIFI_MAX_SCAN_RESULTS];
static int                s_wifi_ucount = 0;

#define MAX_CHANS_PER_SSID  8u
static uint8_t s_wifi_chans[WIFI_MAX_SCAN_RESULTS][MAX_CHANS_PER_SSID];
static int     s_wifi_chan_count[WIFI_MAX_SCAN_RESULTS];

/* RSSI history ring buffer for the detail-panel bar graph.
 * 0 is used as the "no data" sentinel; real RSSI values are always < 0. */
static int8_t s_rssi_history[DISP_WIDTH];
static int    s_graph_head = 0;   /* index of next write position */

/* ---- Bluetooth result cache ----------------------------------------------- */
static bt_scan_result_t s_bt_cache[BT_MAX_SCAN_RESULTS];
static int              s_bt_count = 0;
static int8_t           s_bt_rssi_history[DISP_WIDTH];
static int              s_bt_graph_head = 0;

/* Returns the number of changes made (new devices added or fields updated). */
static int merge_bt_into_cache(const bt_scan_result_t *raw, int count)
{
    int changes = 0;
    for (int i = 0; i < count; i++) {
        int found = -1;
        for (int j = 0; j < s_bt_count; j++) {
            if (memcmp(raw[i].addr, s_bt_cache[j].addr, BT_ADDR_LEN) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (s_bt_cache[found].rssi != raw[i].rssi) {
                s_bt_cache[found].rssi = raw[i].rssi;
                changes++;
            }
            if (raw[i].name[0] != '\0' &&
                strcmp(s_bt_cache[found].name, raw[i].name) != 0) {
                memcpy(s_bt_cache[found].name, raw[i].name, BT_NAME_LEN);
                changes++;
            }
            if (raw[i].tx_power != BT_TX_POWER_UNKNOWN &&
                s_bt_cache[found].tx_power != raw[i].tx_power) {
                s_bt_cache[found].tx_power = raw[i].tx_power;
                changes++;
            }
            if (raw[i].flags != BT_FLAGS_NONE &&
                s_bt_cache[found].flags != raw[i].flags) {
                s_bt_cache[found].flags = raw[i].flags;
                changes++;
            }
            if (raw[i].company_id != BT_COMPANY_NONE &&
                s_bt_cache[found].company_id != raw[i].company_id) {
                s_bt_cache[found].company_id = raw[i].company_id;
                changes++;
            }
        } else if (s_bt_count < BT_MAX_SCAN_RESULTS) {
            s_bt_cache[s_bt_count++] = raw[i];
            changes++;
        }
    }
    return changes;
}

static const char *bt_company_name(uint16_t id)
{
    static const struct { uint16_t id; const char *name; } tbl[] = {
        { 0x0006, "Microsoft"  },
        { 0x004C, "Apple"      },
        { 0x0059, "Nordic Semi"},
        { 0x0075, "Samsung"    },
        { 0x0087, "Garmin"     },
        { 0x00D7, "Arduino"    },
        { 0x00E0, "Google"     },
        { 0x0138, "Fitbit"     },
        { 0x0171, "Amazon"     },
        { 0x02E5, "Espressif"  },
    };
    for (int i = 0; i < (int)(sizeof tbl / sizeof tbl[0]); i++)
        if (tbl[i].id == id) return tbl[i].name;
    return NULL;
}

/* ---- WiFi helpers --------------------------------------------------------- */

/* Map RSSI to a display color per the signal-quality table. */
static uint8_t rssi_color(int16_t rssi)
{
    if (rssi > -67) return COLOR_GREEN;
    if (rssi > -70) return COLOR_YELLOW;
    if (rssi > -80) return COLOR_ORANGE;
    return COLOR_RED;
}

static const char *rssi_label(int16_t rssi)
{
    if (rssi > -67) return "Excellent";
    if (rssi > -70) return "Good";
    if (rssi > -80) return "Limited";
    return "Poor";
}

static const char *auth_label(uint8_t auth_mode)
{
    switch (auth_mode) {
    case 0: return "Open";
    case 1: return "WEP-PSK";
    case 2: return "WPA";
    case 3: return "WEP-PSK/WPA";
    case 4: return "WPA3";
    case 5: return "WEP-PSK/WPA2";
    case 6: return "WPA2/WPA";
    case 7: return "WEP-PSK/WPA/WPA2";
    default: return "Unknown";
    }
}

/*
 * Build a deduplicated index list from raw scan results.
 * Hidden SSIDs (empty name) are skipped.
 * When the same SSID appears more than once, the entry with the stronger
 * (higher, i.e. less-negative) RSSI is kept.
 * Returns the number of unique entries written to out_idx[].
 */
static int build_dedup(const wifi_scan_result_t *results, int count,
                       int *out_idx, int max_out)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (results[i].ssid[0] == '\0') continue;
        int dup = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(results[i].ssid, results[out_idx[j]].ssid) == 0) {
                dup = j;
                break;
            }
        }
        if (dup >= 0) {
            if (results[i].rssi > results[out_idx[dup]].rssi)
                out_idx[dup] = i;
        } else if (n < max_out) {
            out_idx[n++] = i;
        }
    }
    return n;
}

/*
 * Merge a deduplicated scan batch (raw[idx[0..count-1]]) into s_wifi_cache.
 * SSIDs already in the cache get their RSSI/channel updated to the latest
 * reading; new SSIDs are appended (up to WIFI_MAX_SCAN_RESULTS total).
 */
static void merge_into_cache(const wifi_scan_result_t *raw,
                              const int *idx, int count)
{
    for (int i = 0; i < count; i++) {
        const wifi_scan_result_t *r = &raw[idx[i]];
        int found = -1;
        for (int j = 0; j < s_wifi_ucount; j++) {
            if (strcmp(r->ssid, s_wifi_cache[j].ssid) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            /* Update RSSI and auth_mode to latest reading */
            s_wifi_cache[found].rssi      = r->rssi;
            s_wifi_cache[found].auth_mode = r->auth_mode;
            /* Append channel to list if not already present */
            bool ch_seen = false;
            for (int k = 0; k < s_wifi_chan_count[found]; k++) {
                if (s_wifi_chans[found][k] == r->channel) { ch_seen = true; break; }
            }
            if (!ch_seen && s_wifi_chan_count[found] < (int)MAX_CHANS_PER_SSID)
                s_wifi_chans[found][s_wifi_chan_count[found]++] = r->channel;
        } else if (s_wifi_ucount < WIFI_MAX_SCAN_RESULTS) {
            int n = s_wifi_ucount++;
            s_wifi_cache[n]       = *r;
            s_wifi_chan_count[n]  = 1;
            s_wifi_chans[n][0]   = r->channel;
        }
    }
}

/* ---- drawing helpers ------------------------------------------------------ */

static void draw_title(const char *title)
{
    disp_rect_arg_t r = {
        .x = 0, .y = 0, .w = DISP_WIDTH, .h = TITLE_H,
        .color = COLOR_HEADER, .filled = 1, ._pad1 = 0, ._pad2 = 0
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &r);

    int len = (int)strlen(title);
    int x   = ((int)DISP_WIDTH - len * 16) / 2;
    if (x < 4) x = 4;

    disp_text_arg_t t = {
        .x = (uint16_t)x, .y = 2,
        .color = COLOR_GREY, .bg = COLOR_HEADER, .scale = 2, ._pad = 0,
        .str = title
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
}

static void clear_content(void)
{
    disp_rect_arg_t r = {
        .x = 0, .y = TITLE_H,
        .w = DISP_WIDTH, .h = (uint16_t)(DISP_HEIGHT - TITLE_H),
        .color = COLOR_PANEL, .filled = 1, ._pad1 = 0, ._pad2 = 0
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &r);
}

/* Filled right-pointing triangle (▶), red, at the left edge of row_in_view. */
static void draw_selector(int row_in_view)
{
    int half     = (int)TRI_H / 2;
    int y_center = (int)TITLE_H + row_in_view * (int)ROW_H + (int)ROW_H / 2;
    int y_top    = y_center - half;

    for (int r = 0; r < (int)TRI_H; r++) {
        int dist = (r <= half) ? r : (int)TRI_H - 1 - r;
        int w    = (int)TRI_W * dist / half;
        if (w <= 0) continue;
        disp_line_arg_t l = {
            .x0 = (uint16_t)SEL_X,       .y0 = (uint16_t)(y_top + r),
            .x1 = (uint16_t)(SEL_X + w), .y1 = (uint16_t)(y_top + r),
            .color = COLOR_RED, ._pad = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_LINE, &l);
    }
}

static void draw_detail_row(int row_in_view, const char *text, uint8_t color)
{
    int y = (int)TITLE_H + row_in_view * (int)ROW_H + ((int)ROW_H - 16) / 2;
    disp_text_arg_t t = {
        .x = SEL_X, .y = (uint16_t)y,
        .color = color, .bg = COLOR_PANEL, .scale = 2, ._pad = 0,
        .str = text
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
}

static void draw_row(int row_in_view, const char *text, uint8_t color, bool selected)
{
    int y = (int)TITLE_H + row_in_view * (int)ROW_H + ((int)ROW_H - 16) / 2;
    disp_text_arg_t t = {
        .x = TEXT_X, .y = (uint16_t)y,
        .color = color, .bg = COLOR_PANEL, .scale = 2, ._pad = 0,
        .str = text
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
    if (selected)
        draw_selector(row_in_view);
}

static void draw_message(const char *msg)
{
    disp_text_arg_t t = {
        .x = TEXT_X, .y = (uint16_t)(TITLE_H + 2),
        .color = COLOR_GREY, .bg = COLOR_PANEL, .scale = 2, ._pad = 0,
        .str = msg
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
}

static void adjust_scroll(ui_state_t *st, int count)
{
    if (st->sel < st->scroll)
        st->scroll = st->sel;
    if (st->sel >= st->scroll + (int)VISIBLE_ROWS)
        st->scroll = st->sel - (int)VISIBLE_ROWS + 1;
    if (st->scroll > count - (int)VISIBLE_ROWS)
        st->scroll = count - (int)VISIBLE_ROWS;
    if (st->scroll < 0)
        st->scroll = 0;
}

/* ---- screen renderers ----------------------------------------------------- */

static void render_home(ui_state_t *st)
{
    static const char *items[] = {"Bluetooth", "WiFi", "About"};
    enum { HOME_COUNT = 3 };

    draw_title("NetMon V" NETMON_VERSION);
    clear_content();
    for (int i = 0; i < HOME_COUNT; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        draw_row(view, items[i], COLOR_GREY, i == st->sel);
    }
}

static void render_wifi(ui_state_t *st)
{
    draw_title("WiFi Networks");
    clear_content();

    if (s_wifi_ucount == 0) {
        draw_message(st->scan_pending ? "Scanning..." : "No networks found");
        return;
    }

    for (int i = 0; i < s_wifi_ucount; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const wifi_scan_result_t *net = &s_wifi_cache[i];
        char line[20];
        snprintf(line, sizeof(line), "%-11.11s %4d", net->ssid, (int)net->rssi);
        draw_row(view, line, rssi_color(net->rssi), i == st->sel);
    }
}

static void render_rssi_graph(const int8_t *history, int head)
{
    disp_rect_arg_t bg = {
        .x = 0, .y = GRAPH_Y_TOP, .w = DISP_WIDTH, .h = GRAPH_H,
        .color = COLOR_PANEL, .filled = 1, ._pad1 = 0, ._pad2 = 0
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bg);

    for (int x = 0; x < (int)DISP_WIDTH; x++) {
        int gap_rel = (x - head + (int)DISP_WIDTH) % (int)DISP_WIDTH;
        if (gap_rel < (int)GRAPH_GAP_W) continue;

        int8_t rv = history[x];
        if (rv == 0) continue;

        /* Map RSSI (-100..-30 dBm) → bar height 0..100 %. */
        int str = ((int)rv + 100) * 100 / 70;
        if (str < 1)   str = 1;
        if (str > 100) str = 100;

        disp_rect_arg_t bar = {
            .x = (uint16_t)x,
            .y = (uint16_t)((int)GRAPH_Y_BOT - str + 1),
            .w = 1, .h = (uint16_t)str,
            .color = rssi_color((int16_t)rv),
            .filled = 1, ._pad1 = 0, ._pad2 = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bar);
    }
}

static void render_bt(ui_state_t *st)
{
    draw_title("Bluetooth Devices");
    clear_content();

    if (s_bt_count == 0) {
        draw_message(st->bt_scan_pending ? "Scanning..." : "No devices found");
        return;
    }

    for (int i = 0; i < s_bt_count; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const bt_scan_result_t *dev = &s_bt_cache[i];
        const char *nm = dev->name[0] ? dev->name : "Unknown";
        char line[20];
        snprintf(line, sizeof(line), "%-11.11s %4d", nm, (int)dev->rssi);
        draw_row(view, line, rssi_color(dev->rssi), i == st->sel);
    }
}

static void render_bt_detail(ui_state_t *st)
{
    int idx = st->bt_detail_idx;
    if (idx < 0 || idx >= s_bt_count) {
        draw_title("BT Detail");
        clear_content();
        draw_message("No data");
        return;
    }

    const bt_scan_result_t *dev = &s_bt_cache[idx];

    char title[20];
    snprintf(title, sizeof(title), "%.18s", dev->name[0] ? dev->name : "BT Device");
    draw_title(title);
    clear_content();

    /* Row 0: RSSI with signal quality color */
    char rssi_line[20];
    snprintf(rssi_line, sizeof(rssi_line), "RSSI: %ddBm", (int)dev->rssi);
    draw_detail_row(0, rssi_line, rssi_color(dev->rssi));

    /* Row 1: device type */
    char type_line[20];
    snprintf(type_line, sizeof(type_line), "Type: %s",
             dev->type == BT_DEVTYPE_BLE ? "BLE" : "Classic");
    draw_detail_row(1, type_line, COLOR_GREY);

    char addr_str[20];
    const uint8_t *a = dev->addr;
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);

    if (dev->type == BT_DEVTYPE_BLE) {
        /* BLE rows 2-4: TX power, flags, manufacturer company */
        char txpwr_line[20];
        if (dev->tx_power == BT_TX_POWER_UNKNOWN)
            snprintf(txpwr_line, sizeof(txpwr_line), "TxPwr: N/A");
        else
            snprintf(txpwr_line, sizeof(txpwr_line), "TxPwr: %ddBm", (int)dev->tx_power);
        draw_detail_row(2, txpwr_line, COLOR_GREY);

        char flags_line[20];
        if (dev->flags == BT_FLAGS_NONE)
            snprintf(flags_line, sizeof(flags_line), "Flags: N/A");
        else
            snprintf(flags_line, sizeof(flags_line), "Flags: 0x%02X", dev->flags);
        draw_detail_row(3, flags_line, COLOR_GREY);

        char co_line[20];
        if (dev->company_id == BT_COMPANY_NONE) {
            snprintf(co_line, sizeof(co_line), "Co:   N/A");
        } else {
            const char *co = bt_company_name(dev->company_id);
            if (co)
                snprintf(co_line, sizeof(co_line), "Co:   %s", co);
            else
                snprintf(co_line, sizeof(co_line), "Co:   0x%04X", dev->company_id);
        }
        draw_detail_row(4, co_line, COLOR_GREY);

        /* Row 5: MAC address */
        draw_detail_row(5, addr_str, COLOR_GREY);
    } else {
        /* Classic rows 2-3: device class, MAC address */
        char class_line[20];
        snprintf(class_line, sizeof(class_line), "Cls:  %s",
                 bt_devclass_str(dev->dev_class));
        draw_detail_row(2, class_line, COLOR_GREY);

        /* Row 3: MAC address */
        draw_detail_row(3, addr_str, COLOR_GREY);
    }

    render_rssi_graph(s_bt_rssi_history, s_bt_graph_head);
}

static void render_wifi_detail(ui_state_t *st)
{
    int idx = st->detail_idx;
    if (idx < 0 || idx >= s_wifi_ucount) {
        draw_title("WiFi Detail");
        clear_content();
        draw_message("No data");
        return;
    }

    const wifi_scan_result_t *net = &s_wifi_cache[idx];

    /* Title: SSID (truncated to fit title bar at scale 2) */
    char title[20];
    snprintf(title, sizeof(title), "%.18s", net->ssid);
    draw_title(title);
    clear_content();

    /* Row 0: signal strength, colored by RSSI tier */
    char rssi_line[20];
    snprintf(rssi_line, sizeof(rssi_line), "RSSI: %ddBm", (int)net->rssi);
    draw_detail_row(0, rssi_line, rssi_color(net->rssi));

    /* Row 1: channel list */
    char chan_str[20] = "Chan: ";
    int pos = 6;
    for (int i = 0; i < s_wifi_chan_count[idx]; i++) {
        int rem = (int)sizeof(chan_str) - pos;
        if (rem <= 1) break;
        pos += snprintf(chan_str + pos, (size_t)rem,
                        i == 0 ? "%d" : ",%d", (int)s_wifi_chans[idx][i]);
    }
    draw_detail_row(1, chan_str, COLOR_GREY);

    /* Row 2: security type */
    char sec_str[24];
    snprintf(sec_str, sizeof(sec_str), "Sec: %s", auth_label(net->auth_mode));
    draw_detail_row(2, sec_str, COLOR_GREY);

    /* Row 3: BSSID — fits as one line at scale 2 starting from left margin */
    char bssid_str[20];
    const uint8_t *b = net->bssid;
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    draw_detail_row(3, bssid_str, COLOR_GREY);

    render_rssi_graph(s_rssi_history, s_graph_head);
}

static void render_about(void)
{
    draw_title("About");
    clear_content();

    uint32_t used = 0u, free_bytes = 0u, largest = 0u;
    kmem_stats(&used, &free_bytes, &largest);

    char lines[6][20];
    snprintf(lines[0], sizeof(lines[0]), "NetMon: v" NETMON_VERSION);
    snprintf(lines[1], sizeof(lines[1]), "picoOS: v%d.%d.%d",
             PICOOS_VERSION_MAJOR, PICOOS_VERSION_MINOR, PICOOS_VERSION_EDIT);
    snprintf(lines[2], sizeof(lines[2]), "Memory:");
    snprintf(lines[3], sizeof(lines[3]), " Free: %5u B", (unsigned)free_bytes);
    snprintf(lines[4], sizeof(lines[4]), " Used: %5u B", (unsigned)used);
    snprintf(lines[5], sizeof(lines[5]), " Max:  %5u B", (unsigned)largest);

    for (int i = 0; i < 6; i++) {
        int y = (int)TITLE_H + i * (int)ROW_H + ((int)ROW_H - 16) / 2;
        disp_text_arg_t t = {
            .x = TEXT_X, .y = (uint16_t)y,
            .color = COLOR_GREY, .bg = COLOR_PANEL, .scale = 2, ._pad = 0,
            .str = lines[i]
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
    }
}

static void render_screen(ui_state_t *st)
{
    switch (st->screen) {
    case SCR_HOME:        render_home(st);        break;
    case SCR_WIFI:        render_wifi(st);        break;
    case SCR_BT:          render_bt(st);          break;
    case SCR_BT_DETAIL:   render_bt_detail(st);   break;
    case SCR_ABOUT:       render_about();         break;
    case SCR_WIFI_DETAIL: render_wifi_detail(st); break;
    }
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
}

/* ---- navigation ----------------------------------------------------------- */

static void enter_screen(ui_state_t *st, screen_t s)
{
    st->prev_sel    = st->sel;
    st->prev_scroll = st->scroll;
    st->screen       = s;
    st->sel          = 0;
    st->scroll       = 0;
    st->dirty        = true;
    st->scan_pending = false;

    if (s == SCR_WIFI) {
        s_wifi_ucount    = 0;
        memset(s_wifi_chan_count, 0, sizeof(s_wifi_chan_count));
        st->rescan_ticks = 0;
        wifi_scan();
        st->scan_pending = true;
    }
    if (s == SCR_BT) {
        s_bt_count           = 0;
        st->bt_rescan_ticks  = 0;
        bt_scan();
        st->bt_scan_pending  = true;
    }
}

static void back_to_home(ui_state_t *st)
{
    st->screen          = SCR_HOME;
    st->sel             = st->prev_sel;
    st->scroll          = st->prev_scroll;
    st->rescan_ticks    = 0;
    st->bt_rescan_ticks = 0;
    st->dirty           = true;
    st->scan_pending    = false;
    st->bt_scan_pending = false;
}

static void handle_input(ui_state_t *st, uint8_t pressed)
{
    if (!pressed) return;

    switch (st->screen) {
    case SCR_HOME: {
        enum { HOME_COUNT = 3 };
        if (pressed & DISP_BTN_A) {
            if (st->sel > 0) { st->sel--; st->dirty = true; }
        }
        if (pressed & DISP_BTN_B) {
            if (st->sel < HOME_COUNT - 1) { st->sel++; st->dirty = true; }
        }
        if (pressed & DISP_BTN_X) {
            switch (st->sel) {
            case 0: enter_screen(st, SCR_BT);    break;
            case 1: enter_screen(st, SCR_WIFI);  break;
            case 2: enter_screen(st, SCR_ABOUT); break;
            }
        }
        break;
    }
    case SCR_WIFI: {
        if (!st->scan_pending && s_wifi_ucount > 0) {
            if (pressed & DISP_BTN_A) {
                if (st->sel > 0) {
                    st->sel--;
                    adjust_scroll(st, s_wifi_ucount);
                    st->dirty = true;
                }
            }
            if (pressed & DISP_BTN_B) {
                if (st->sel < s_wifi_ucount - 1) {
                    st->sel++;
                    adjust_scroll(st, s_wifi_ucount);
                    st->dirty = true;
                }
            }
            if (pressed & DISP_BTN_X) {
                st->wifi_sel    = st->sel;
                st->wifi_scroll = st->scroll;
                st->detail_idx  = st->sel;
                st->screen      = SCR_WIFI_DETAIL;
                st->dirty       = true;
                memset(s_rssi_history, 0, sizeof(s_rssi_history));
                s_graph_head = 0;
            }
        }
        if (pressed & DISP_BTN_Y) back_to_home(st);
        break;
    }
    case SCR_WIFI_DETAIL:
        if (pressed & DISP_BTN_Y) {
            st->screen = SCR_WIFI;
            st->sel    = st->wifi_sel;
            st->scroll = st->wifi_scroll;
            st->dirty  = true;
        }
        break;
    case SCR_BT: {
        if (s_bt_count > 0) {
            if (pressed & DISP_BTN_A) {
                if (st->sel > 0) {
                    st->sel--;
                    adjust_scroll(st, s_bt_count);
                    st->dirty = true;
                }
            }
            if (pressed & DISP_BTN_B) {
                if (st->sel < s_bt_count - 1) {
                    st->sel++;
                    adjust_scroll(st, s_bt_count);
                    st->dirty = true;
                }
            }
            if (pressed & DISP_BTN_X) {
                st->bt_sel        = st->sel;
                st->bt_scroll     = st->scroll;
                st->bt_detail_idx = st->sel;
                st->screen        = SCR_BT_DETAIL;
                st->dirty         = true;
                memset(s_bt_rssi_history, 0, sizeof(s_bt_rssi_history));
                s_bt_graph_head   = 0;
            }
        }
        if (pressed & DISP_BTN_Y) back_to_home(st);
        break;
    }
    case SCR_BT_DETAIL:
        if (pressed & DISP_BTN_Y) {
            st->screen = SCR_BT;
            st->sel    = st->bt_sel;
            st->scroll = st->bt_scroll;
            st->dirty  = true;
        }
        break;
    case SCR_ABOUT:
        if (pressed & DISP_BTN_Y) back_to_home(st);
        break;
    }
}

/* ---- entry point ---------------------------------------------------------- */

void netmon_entry(void *arg)
{
    (void)arg;

    dev_open(DEV_DISPLAY);

    uint8_t bg = COLOR_PANEL;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

    ui_state_t st = {
        .screen           = SCR_HOME,
        .sel              = 0,
        .scroll           = 0,
        .prev_sel         = 0,
        .prev_scroll      = 0,
        .wifi_sel         = 0,
        .wifi_scroll      = 0,
        .detail_idx       = 0,
        .rescan_ticks     = 0,
        .bt_sel           = 0,
        .bt_scroll        = 0,
        .bt_detail_idx    = 0,
        .bt_rescan_ticks  = 0,
        .dirty            = true,
        .scan_pending     = false,
        .bt_scan_pending  = false,
    };
    uint8_t prev_btns = 0u;

    for (;;) {
        if (st.screen == SCR_WIFI || st.screen == SCR_WIFI_DETAIL) {
            if (st.scan_pending && wifi_scan_is_done()) {
                const wifi_scan_result_t *raw = NULL;
                int raw_count = 0;
                wifi_get_scan_results(&raw, &raw_count);
                int tmp_idx[WIFI_MAX_SCAN_RESULTS];
                int new_ucount = build_dedup(raw, raw_count, tmp_idx, WIFI_MAX_SCAN_RESULTS);
                merge_into_cache(raw, tmp_idx, new_ucount);
                if (st.screen == SCR_WIFI) {
                    if (st.sel >= s_wifi_ucount)
                        st.sel = s_wifi_ucount > 0 ? s_wifi_ucount - 1 : 0;
                    adjust_scroll(&st, s_wifi_ucount);
                }
                /* Record a graph data point when the detail panel is open */
                if (st.screen == SCR_WIFI_DETAIL &&
                    st.detail_idx >= 0 && st.detail_idx < s_wifi_ucount) {
                    s_rssi_history[s_graph_head] =
                        (int8_t)s_wifi_cache[st.detail_idx].rssi;
                    s_graph_head = (s_graph_head + 1) % (int)DISP_WIDTH;
                }
                st.scan_pending = false;
                st.dirty        = true;
                st.rescan_ticks = RESCAN_DELAY_TICKS;
            } else if (!st.scan_pending && st.rescan_ticks > 0) {
                if (--st.rescan_ticks == 0) {
                    wifi_scan();
                    st.scan_pending = true;
                }
            }
        }
        if (st.screen == SCR_BT || st.screen == SCR_BT_DETAIL) {
            if (st.bt_scan_pending) {
                /* Live update: merge partial results on every tick so devices
                 * appear as they are discovered rather than at scan end. */
                const bt_scan_result_t *raw = NULL;
                int raw_count = 0;
                bt_get_scan_results(&raw, &raw_count);
                if (raw_count > 0) {
                    int old_count = s_bt_count;
                    if (merge_bt_into_cache(raw, raw_count) > 0) {
                        if (st.screen == SCR_BT && s_bt_count != old_count) {
                            if (st.sel >= s_bt_count)
                                st.sel = s_bt_count > 0 ? s_bt_count - 1 : 0;
                            adjust_scroll(&st, s_bt_count);
                        }
                        st.dirty = true;
                    }
                }
                /* Scan complete: record RSSI graph point and schedule rescan. */
                if (bt_scan_is_done()) {
                    if (st.screen == SCR_BT_DETAIL &&
                        st.bt_detail_idx >= 0 && st.bt_detail_idx < s_bt_count) {
                        s_bt_rssi_history[s_bt_graph_head] =
                            (int8_t)s_bt_cache[st.bt_detail_idx].rssi;
                        s_bt_graph_head = (s_bt_graph_head + 1) % (int)DISP_WIDTH;
                    }
                    st.bt_scan_pending  = false;
                    st.dirty            = true;
                    st.bt_rescan_ticks  = BT_RESCAN_DELAY_TICKS;
                }
            } else if (st.bt_rescan_ticks > 0) {
                /* No cache clear on periodic rescan — accumulate devices across
                 * cycles; merge updates RSSI when a device reappears. */
                if (--st.bt_rescan_ticks == 0) {
                    bt_scan();
                    st.bt_scan_pending = true;
                }
            }
        }
        uint8_t btns    = 0u;
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_GET_BTNS, &btns);
        uint8_t pressed = (uint8_t)(btns & ~prev_btns);  /* rising-edge detect */
        prev_btns = btns;

        handle_input(&st, pressed);

        if (st.dirty) {
            render_screen(&st);
            st.dirty = false;
        }

        sys_sleep(50);
    }
}
