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
 * netmon.c — NetMon display UI application
 *
 * Screens: Home → WiFi (Scan | Channels) | Bluetooth (Scan | Beacons | Pairs) |
 *               Watch | About
 * Buttons: A=up  B=down  X=select  Y=back
 *   WiFi detail: A watch/unwatch, B scroll (wraps), X per-SSID AP list
 *   BT detail:   X finder (in the finder, X watch/unwatch)
 *   Pairs:       X opens a pair (both devices, evidence, RSSI traces)
 *   Channels:    X bar chart <-> selected channel's history
 *   Watch:       X remove the selected entry
 */

#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/sync.h"
#include "kernel/dev.h"
#include "kernel/mem.h"
#include "kernel/fs.h"
#include "kernel/vfs.h"
#include "drivers/display.h"
#include "netmon.h"
#include "kernel/wifi.h"
#include "kernel/bluetooth.h"
#include "oui.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- scan timing ---------------------------------------------------------- *
 * picoOS continuous scanning delivers one window at least every second
 * (WIFI_WINDOW_MAX_MS / BT_WINDOW_MS), so there are no rescan pauses or scan
 * timeouts here.  A BT device stays "seen" for a few windows after it was
 * last heard, so the graph does not show a hole for every missed packet:
 *   - BLE: devices advertising every 1-2 s regularly miss a 1 s window.
 *   - Classic: only heard while an inquiry runs, which picoOS starts once
 *     every BT_INQUIRY_PERIOD_MS, so hold for a whole inquiry period.        */
#define BT_SEEN_HOLD_BLE      3u
#define BT_SEEN_HOLD_CLASSIC  (BT_INQUIRY_PERIOD_MS / BT_WINDOW_MS + 1u)
#define SCAN_RETRY_MS   200u      /* wait before retrying a refused start     */

/* ---- graph geometry ------------------------------------------------------- */
#define GRAPH_H      56u
#define GRAPH_Y_BOT ((uint16_t)(DISP_HEIGHT - 1u))
#define GRAPH_Y_TOP ((uint16_t)(DISP_HEIGHT - GRAPH_H))
#define GRAPH_GAP_W   8u          /* blank gap starting at the write head — marks current position */

/* ---- layout --------------------------------------------------------------- */
#define TITLE_H      20u
#define ROW_H        20u
#define DETAIL_ROW_H 18u          /* tighter rows on detail panels (16 px font) */
#define SEL_X         4u
#define TEXT_X       18u
#define TRI_H        13u
#define TRI_W         9u
#define VISIBLE_ROWS  ((DISP_HEIGHT - TITLE_H) / ROW_H)
#define DETAIL_ROWS   ((GRAPH_Y_TOP - TITLE_H) / DETAIL_ROW_H)  /* above graph */
#define BT_DETAIL_MAX_LINES 28u
#define WIFI_DETAIL_MAX_LINES 12u

/* ---- state ---------------------------------------------------------------- */
typedef enum { SCR_HOME = 0, SCR_WIFI, SCR_BT, SCR_BT_DETAIL, SCR_ABOUT, SCR_WIFI_DETAIL,
               SCR_CHANNELS, SCR_WIFI_APS, SCR_BT_FINDER, SCR_WATCH,
               SCR_WIFI_MENU, SCR_BT_MENU, SCR_BT_PAIRS, SCR_BT_PAIR } screen_t;

enum { MENU_HOME = 0, MENU_WIFI, MENU_BT, MENU_COUNT };

typedef struct {
    screen_t screen;
    int      sel;
    int      scroll;
    int      menu_sel[MENU_COUNT]; /* each menu's cursor, kept while away */
    int      wifi_sel;      /* WiFi list cursor saved when entering detail panel */
    int      wifi_scroll;
    int      detail_idx;    /* s_wifi_cache index shown in the detail panel */
    int      bt_sel;        /* BT list cursor saved when entering detail panel */
    int      bt_scroll;
    int      bt_detail_idx; /* s_bt_cache index shown in the BT detail panel */
    uint8_t  bt_detail_addr[BT_ADDR_LEN]; /* its address, to spot a reused slot */
    int      detail_scroll; /* first row shown on a WiFi / BT detail panel */
    bool     chan_hist;     /* Channels: history graph instead of bars */
    bool     bt_filter;     /* BT list shows beacons and trackers only */
    int      pair_sel;      /* Pairs list cursor saved when entering a pair */
    int      pair_scroll;
    uint8_t  pair_addr[2][BT_ADDR_LEN]; /* the pair open in the pair panel */
    bool     dirty;
} ui_state_t;

/* ---- cross-thread state (main thread ↔ scan worker) ---------------------- *
 * kmutex_t rather than spinlock_t: the two threads run at different
 * priorities, so a spinning waiter could starve a preempted holder, and
 * spinlock_init() permanently claims one of the scarce RP2040 HW spinlocks
 * on every launch.  kmutex_t blocks the waiter and draws from the kernel's
 * shared spinlock pool.                                                       */
static kmutex_t          s_wifi_lock;        /* guards wifi cache + flags    */
static kmutex_t          s_bt_lock;          /* guards bt cache + flags      */
static bool              s_new_wifi_data  = false;   /* s_wifi_lock */
static bool              s_new_bt_data    = false;   /* s_bt_lock   */
static volatile screen_t s_active_screen  = SCR_HOME; /* main writes, worker reads */
static bool              s_scanning_wifi  = false;   /* s_wifi_lock */
static bool              s_scanning_bt    = false;   /* s_bt_lock   */

/* ---- WiFi result cache ---------------------------------------------------- */

static wifi_scan_result_t s_wifi_cache[WIFI_MAX_SCAN_RESULTS];
static int                s_wifi_ucount = 0;

#define MAX_CHANS_PER_SSID  8u
static uint8_t s_wifi_chans[WIFI_MAX_SCAN_RESULTS][MAX_CHANS_PER_SSID];
static int     s_wifi_chan_count[WIFI_MAX_SCAN_RESULTS];
/* True if the entry was heard in the most recent scan; false means its
 * cached RSSI is stale.  Guarded by s_wifi_lock.                            */
static bool    s_wifi_seen[WIFI_MAX_SCAN_RESULTS];
/* BSSIDs sharing the SSID in the most recent scan (mesh / multi-AP). */
static int     s_wifi_aps[WIFI_MAX_SCAN_RESULTS];
/* Window an entry was last heard in; when the cache is full, the entry
 * heard longest ago (and not within WIFI_EVICT_WINDOWS) makes room. */
#define WIFI_EVICT_WINDOWS  5u
static uint32_t s_wifi_last_win[WIFI_MAX_SCAN_RESULTS];

/* Every BSSID of one network (the one open in the detail panel), heard
 * within AP_HOLD_WINDOWS windows.  s_ap_key names the network; the worker
 * fills s_ap_list[] from each raw window.  Guarded by s_wifi_lock.          */
#define MAX_APS          8
#define AP_HOLD_WINDOWS  3u
typedef struct {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t last_win;
} ap_entry_t;
static wifi_scan_result_t s_ap_key;
static bool               s_ap_key_set = false;
static ap_entry_t         s_ap_list[MAX_APS];
static int                s_ap_count   = 0;

/* The network shown in the WiFi detail / AP panels, to spot its cache slot
 * being reused for another network.  Main thread only.                      */
static wifi_scan_result_t s_detail_key;

/* ---- channel survey ------------------------------------------------------- *
 * Per-channel figures for the Channels screen, one slot per scan window over
 * the last SURVEY_WINDOWS windows.  Built from the raw window (every BSSID,
 * hidden SSIDs too), not from the SSID-deduplicated cache, so it counts what
 * is actually on the air.  Guarded by s_wifi_lock.                           */
#define SURVEY_CHANS    13        /* 2.4 GHz channels 1..13                 */
#define SURVEY_WINDOWS   8        /* ~8 s of windows                        */
#define SURVEY_STRONG  (-70)      /* APs above this count double in load    */

typedef struct {
    uint8_t aps;        /* BSSIDs heard on the channel                      */
    uint8_t strong;     /* of those, how many above SURVEY_STRONG           */
    int8_t  best_rssi;  /* strongest RSSI, 0 = none                         */
    int8_t  noise;      /* mean firmware noise floor in dBm, 0 = unknown    */
} chan_win_t;

static chan_win_t s_survey[SURVEY_WINDOWS][SURVEY_CHANS];
static int        s_survey_head = 0;

/* Channels history view: one column per window for the selected channel.
 * Load is stored + 1 (0 = no sample); noise in dBm (0 = unknown).  Main
 * thread only.                                                              */
static uint8_t s_chan_hist_load[DISP_WIDTH];
static int8_t  s_chan_hist_noise[DISP_WIDTH];
static int     s_chan_hist_head = 0;

/* RSSI history ring buffer for the detail-panel bar graph.
 * 0 is used as the "no data" sentinel; real RSSI values are always < 0. */
static int8_t s_rssi_history[DISP_WIDTH];
static int    s_graph_head = 0;   /* index of next write position */

/* ---- Bluetooth result cache ----------------------------------------------- */
static bt_scan_result_t s_bt_cache[BT_MAX_SCAN_RESULTS];
static int              s_bt_count = 0;
/* True if the entry was heard with a valid RSSI within its hold
 * (BT_SEEN_HOLD_BLE / _CLASSIC windows); false means its cached RSSI is stale.  s_bt_last_win[] holds the
 * window number it was last heard in (0 = never).  Guarded by s_bt_lock.    */
static bool             s_bt_seen[BT_MAX_SCAN_RESULTS];
static uint32_t         s_bt_last_win[BT_MAX_SCAN_RESULTS];
static uint32_t         s_bt_first_win[BT_MAX_SCAN_RESULTS]; /* first heard   */
static uint32_t         s_bt_win;           /* latest window merged; s_bt_lock */
/* Set by the worker when a window has been merged; the UI adds one graph
 * column per window.  Guarded by s_bt_lock.                                 */
static bool             s_bt_scan_end = false;
static int8_t           s_bt_rssi_history[DISP_WIDTH];
static int              s_bt_graph_head = 0;

/* ---- co-presence history (Pairs screen) ----------------------------------- *
 * One byte per cached device per window, over the last CORR_WINDOWS windows:
 * 0 = absent, CORR_HELD = present (within its seen hold) but not heard this
 * window, else the RSSI heard (always < 0).  Devices carried by one person
 * (a phone and its watch or earbuds) arrive, leave and move together, so
 * their presence and RSSI correlate over time.  A slot's row restarts when
 * the slot is filled with another device.  Ring position = window number %
 * CORR_WINDOWS.  Guarded by s_bt_lock.                                       */
#define CORR_WINDOWS      128     /* ~2 min of 1 s windows                  */
#define CORR_HELD         1
#define CORR_MIN_WINDOWS  20      /* windows recorded before any scoring    */
#define CORR_MIN_SAMPLES  12      /* windows both heard, for RSSI corr.     */
#define CORR_MIN_SCORE    40      /* listed from this score (0..100)        */
#define CORR_MAX_PAIRS    16
#define CORR_NONE         (-1000) /* correlation undefined (no variation)   */

static int8_t   s_corr[BT_MAX_SCAN_RESULTS][CORR_WINDOWS];
static uint32_t s_corr_win = 0u;  /* latest window recorded                 */
static int      s_corr_n   = 0;   /* windows recorded, up to CORR_WINDOWS   */

static void corr_reset(void)
{
    memset(s_corr, 0, sizeof(s_corr));
    s_corr_win = 0u;
    s_corr_n   = 0;
}

/* Record window `win` for every slot, once s_bt_seen[] is up to date.
 * Windows lost in between (a slow reader) are recorded as absent. */
static void corr_record(uint32_t win)
{
    uint32_t gap = s_corr_n > 0 ? win - s_corr_win : 1u;
    if (gap == 0u) return;
    for (uint32_t k = 1u; k < gap && k <= CORR_WINDOWS; k++) {
        int pos = (int)((s_corr_win + k) % CORR_WINDOWS);
        for (int j = 0; j < BT_MAX_SCAN_RESULTS; j++) s_corr[j][pos] = 0;
    }
    int pos = (int)(win % CORR_WINDOWS);
    for (int j = 0; j < BT_MAX_SCAN_RESULTS; j++) {
        int8_t v = 0;
        if (j < s_bt_count && s_bt_seen[j]) {
            v = CORR_HELD;
            if (s_bt_last_win[j] == win)
                v = s_bt_cache[j].rssi < 0 ? s_bt_cache[j].rssi : (int8_t)-1;
        }
        s_corr[j][pos] = v;
    }
    s_corr_win = win;
    uint32_t n = (uint32_t)s_corr_n + gap;
    s_corr_n   = n > CORR_WINDOWS ? CORR_WINDOWS : (int)n;
}

/* Copy the fields a window reported into a cached entry; a field the window
 * did not carry keeps its cached value.  Returns the number changed. */
#define MERGE_FIELD(f, none)                                  \
    do {                                                      \
        if (src->f != (none) && dst->f != src->f) {           \
            dst->f = src->f;                                  \
            n++;                                              \
        }                                                     \
    } while (0)

static int merge_bt_fields(bt_scan_result_t *dst, const bt_scan_result_t *src)
{
    int n = 0;
    if (src->name[0] != '\0' && strcmp(dst->name, src->name) != 0) {
        memcpy(dst->name, src->name, BT_NAME_LEN);
        n++;
    }
    MERGE_FIELD(tx_power,     BT_TX_POWER_UNKNOWN);
    MERGE_FIELD(flags,        BT_FLAGS_NONE);
    MERGE_FIELD(service_uuid, BT_SERVICE_NONE);
    MERGE_FIELD(uuid32,       BT_UUID32_NONE);
    MERGE_FIELD(appearance,   BT_APPEARANCE_NONE);
    MERGE_FIELD(adv_type,     BT_ADV_TYPE_NONE);
    MERGE_FIELD(addr_type,    BT_ADDR_TYPE_NONE);
    /* Data travels with the ID that marks it present. */
    if (src->company_id != BT_COMPANY_NONE &&
        (dst->company_id != src->company_id || dst->mfr_data_len != src->mfr_data_len ||
         memcmp(dst->mfr_data, src->mfr_data, src->mfr_data_len) != 0)) {
        dst->company_id   = src->company_id;
        dst->mfr_data_len = src->mfr_data_len;
        memcpy(dst->mfr_data, src->mfr_data, sizeof(dst->mfr_data));
        n++;
    }
    if (src->svc_data_uuid != BT_SERVICE_NONE &&
        (dst->svc_data_uuid != src->svc_data_uuid || dst->svc_data_len != src->svc_data_len ||
         memcmp(dst->svc_data, src->svc_data, src->svc_data_len) != 0)) {
        dst->svc_data_uuid = src->svc_data_uuid;
        dst->svc_data_len  = src->svc_data_len;
        memcpy(dst->svc_data, src->svc_data, sizeof(dst->svc_data));
        n++;
    }
    if (src->did_source != BT_DID_NONE) {
        dst->did_source  = src->did_source;
        dst->did_vendor  = src->did_vendor;
        dst->did_product = src->did_product;
        dst->did_version = src->did_version;
    }
    dst->pkt_count = src->pkt_count;   /* reports in the latest window */
    return n;
}

/* Merge one scan window (number `win`) into the cache and rebuild
 * s_bt_seen[].  Returns the number of changes made (new devices added or
 * fields updated). */
static int merge_bt_into_cache(const bt_scan_result_t *raw, int count,
                               uint32_t win)
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
        bool valid = raw[i].rssi != BT_RSSI_UNKNOWN;
        if (found >= 0) {
            if (valid && s_bt_cache[found].rssi != raw[i].rssi) {
                s_bt_cache[found].rssi = raw[i].rssi;
                changes++;
            }
            if (valid) s_bt_last_win[found] = win;
            changes += merge_bt_fields(&s_bt_cache[found], &raw[i]);
        } else if (s_bt_count < BT_MAX_SCAN_RESULTS) {
            s_bt_cache[s_bt_count]     = raw[i];
            s_bt_last_win[s_bt_count]  = valid ? win : 0u;
            s_bt_first_win[s_bt_count] = win;
            memset(s_corr[s_bt_count], 0, CORR_WINDOWS);
            s_bt_count++;           /* increment after full write */
            changes++;
        } else {
            /* Full: reuse the slot of the device heard longest ago, as long
             * as it is no longer seen.  Phones rotate BLE addresses, so
             * without this stale entries would lock new devices out. */
            int old = -1;
            for (int j = 0; j < s_bt_count; j++) {
                if (s_bt_seen[j] || s_bt_last_win[j] == win) continue;
                if (old < 0 || s_bt_last_win[j] < s_bt_last_win[old]) old = j;
            }
            if (old >= 0) {
                s_bt_cache[old]     = raw[i];
                s_bt_last_win[old]  = valid ? win : 0u;
                s_bt_first_win[old] = win;
                s_bt_seen[old]      = valid;
                memset(s_corr[old], 0, CORR_WINDOWS);
                changes++;
            }
        }
    }
    for (int j = 0; j < s_bt_count; j++) {
        uint32_t hold = s_bt_cache[j].type == BT_DEVTYPE_CLASSIC
                            ? BT_SEEN_HOLD_CLASSIC : BT_SEEN_HOLD_BLE;
        s_bt_seen[j] = s_bt_last_win[j] != 0u &&
                       win - s_bt_last_win[j] < hold;
    }
    corr_record(win);
    s_bt_win = win;
    return changes;
}

static const char *bt_company_name(uint16_t id)
{
    static const struct { uint16_t id; const char *name; } tbl[] = {
        { 0x0006, "Microsoft"  },
        { 0x000D, "TI"         },
        { 0x000F, "Broadcom"   },
        { 0x001D, "Qualcomm"   },
        { 0x0030, "ST Micro"   },
        { 0x0046, "MediaTek"   },
        { 0x004C, "Apple"      },
        { 0x0059, "Nordic Semi"},
        { 0x0067, "GN (Jabra)" },
        { 0x0075, "Samsung"    },
        { 0x0087, "Garmin"     },
        { 0x009E, "Bose"       },
        { 0x00D7, "Arduino"    },
        { 0x00E0, "Google"     },
        { 0x012D, "Sony"       },
        { 0x0131, "Cypress"    },
        { 0x0138, "Fitbit"     },
        { 0x0157, "Huami"      },
        { 0x0171, "Amazon"     },
        { 0x01DA, "Logitech"   },
        { 0x02E5, "Espressif"  },
        { 0x02FF, "Silicon Lab"},
        { 0x038F, "Xiaomi"     },
        { 0x0499, "Ruuvi"      },
        { 0x4C42, "Epson"      },
    };
    for (int i = 0; i < (int)(sizeof tbl / sizeof tbl[0]); i++)
        if (tbl[i].id == id) return tbl[i].name;
    return NULL;
}

/* Name a 16-bit service UUID: SIG-defined services (0x18xx) and UUIDs the
 * Bluetooth SIG assigned to member companies (0xFCxx-0xFExx). */
static const char *bt_service_name(uint16_t uuid)
{
    static const struct { uint16_t uuid; const char *name; } tbl[] = {
        { 0x1800, "GAP"        },
        { 0x1801, "GATT"       },
        { 0x1802, "Imm. Alert" },
        { 0x1803, "Link Loss"  },
        { 0x1805, "Time"       },
        { 0x1808, "Glucose"    },
        { 0x1809, "Thermometer"},
        { 0x180A, "Device Info"},
        { 0x180D, "Heart Rate" },
        { 0x180F, "Battery"    },
        { 0x1810, "Blood Press"},
        { 0x1812, "HID"        },
        { 0x1814, "Run Speed"  },
        { 0x1816, "Cycling"    },
        { 0x1818, "Cycl. Power"},
        { 0x181A, "Env Sensing"},
        { 0x181C, "User Data"  },
        { 0x181D, "Weight"     },
        { 0x1826, "Fitness"    },
        { 0x184E, "LE Audio"   },
        { 0xFCD2, "BTHome"     },
        { 0xFCF1, "Google"     },
        { 0xFD5A, "Samsung Tag"},
        { 0xFD6F, "Exposure N."},
        { 0xFE03, "Amazon"     },
        { 0xFE07, "Sonos"      },
        { 0xFE0F, "Signify Hue"},
        { 0xFE2C, "Fast Pair"  },
        { 0xFE59, "Nordic DFU" },
        { 0xFE95, "Xiaomi"     },
        { 0xFE96, "Tesla"      },
        { 0xFE97, "Tesla"      },
        { 0xFE9F, "Google"     },
        { 0xFEAA, "Eddystone"  },
        { 0xFEAF, "Nest Labs"  },
        { 0xFEB0, "Nest Labs"  },
        { 0xFEBE, "Bose"       },
        { 0xFEED, "Tile"       },
        { 0xFEF3, "Google"     },
    };
    for (int i = 0; i < (int)(sizeof tbl / sizeof tbl[0]); i++)
        if (tbl[i].uuid == uuid) return tbl[i].name;
    return NULL;
}

/* Minor device class of a Classic Class of Device (bits 2-7), or NULL. */
static const char *bt_cod_minor_str(uint32_t cod)
{
    unsigned major = (cod >> 8) & 0x1Fu;
    unsigned minor = (cod >> 2) & 0x3Fu;
    static const char *const computer[] = {
        NULL, "Desktop", "Server", "Laptop", "Handheld", "Palm", "Wearable",
        "Tablet" };
    static const char *const phone[] = {
        NULL, "Cellular", "Cordless", "Smartphone", "Modem", "ISDN" };
    static const char *const audio[] = {
        NULL, "Headset", "Hands-free", NULL, "Microphone", "Speaker",
        "Headphones", "Portable", "Car audio", "Set-top box", "HiFi", "VCR",
        "Video camera", "Camcorder", "Video mon.", "Video disp.",
        "Video conf.", NULL, "Game/toy" };
    static const char *const wearable[] = {
        NULL, "Wristwatch", "Pager", "Jacket", "Helmet", "Glasses", "Pin" };
    static const char *const toy[] = {
        NULL, "Robot", "Vehicle", "Doll", "Controller", "Game" };
    static const char *const health[] = {
        NULL, "BP monitor", "Thermometer", "Scale", "Glucose", "Pulse oxim.",
        "Heart rate", "Health disp.", "Step counter" };
#define PICK(t) (minor < sizeof(t) / sizeof((t)[0]) ? (t)[minor] : NULL)
    switch (major) {
    case 1: return PICK(computer);
    case 2: return PICK(phone);
    case 4: return PICK(audio);
    case 5:   /* peripheral: bits 6-7 keyboard/pointer, 2-5 the type */
        switch (minor >> 4) {
        case 1:  return "Keyboard";
        case 2:  return "Mouse";
        case 3:  return "Kbd+mouse";
        }
        switch (minor & 0x0Fu) {
        case 1:  return "Joystick";
        case 2:  return "Gamepad";
        case 3:  return "Remote";
        case 5:  return "Digitizer";
        case 6:  return "Card reader";
        case 7:  return "Pen";
        }
        return NULL;
    case 6:   /* imaging: a bit mask in bits 4-7 */
        if (minor & 0x20u) return "Printer";
        if (minor & 0x10u) return "Scanner";
        if (minor & 0x08u) return "Camera";
        if (minor & 0x04u) return "Display";
        return NULL;
    case 7: return PICK(wearable);
    case 8: return PICK(toy);
    case 9: return PICK(health);
    }
#undef PICK
    return NULL;
}

/* Service classes a Classic device advertises in CoD bits 13-23, as short
 * names separated by spaces.  Writes "" when there are none. */
static void bt_cod_services(uint32_t cod, char *buf, size_t len)
{
    static const struct { uint8_t bit; const char *name; } tbl[] = {
        { 21, "Audio" }, { 22, "Phone" }, { 17, "Net"   }, { 20, "ObjXfer" },
        { 18, "Render"}, { 19, "Capture"}, { 16, "Posn" }, { 23, "Info"    },
        { 14, "LEAudio"},
    };
    size_t pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < (int)(sizeof tbl / sizeof tbl[0]); i++) {
        if (!(cod & (1ul << tbl[i].bit))) continue;
        int w = snprintf(buf + pos, len - pos, pos ? " %s" : "%s", tbl[i].name);
        if (w < 0 || (size_t)w >= len - pos) break;
        pos += (size_t)w;
    }
}

/* GAP Appearance (AD 0x19): category in bits 6-15, subcategory in 0-5. */
static const char *bt_appearance_str(uint16_t app)
{
    unsigned cat = app >> 6, sub = app & 0x3Fu;
    if (cat == 15) {   /* HID */
        static const char *const hid[] = {
            "HID", "Keyboard", "Mouse", "Joystick", "Gamepad", "Digitizer",
            "Card reader", "Pen", "Barcode scan" };
        return sub < sizeof hid / sizeof hid[0] ? hid[sub] : "HID";
    }
    static const char *const cats[] = {
        NULL, "Phone", "Computer", "Watch", "Clock", "Display", "Remote",
        "Eyeglasses", "Tag", "Keyring", "Media player", "Barcode scan",
        "Thermometer", "Heart rate", "Blood press.", "HID", "Glucose",
        "Run/walk", "Cycling", "Control dev", "Network dev", "Sensor",
        "Light", "Fan", "HVAC", "Air cond.", "Humidifier", "Heating",
        "Access ctrl", "Motorized", "Power dev", "Light source",
        "Window cover", "Audio sink", "Audio source", "Vehicle",
        "Appliance", "Earbuds", "Aircraft", "AV equipment", "Display eq.",
        "Hearing aid", "Gaming", "Signage" };
    if (cat < sizeof cats / sizeof cats[0]) return cats[cat];
    switch (cat) {
    case 49: return "Pulse oxim.";
    case 50: return "Weight scale";
    case 51: return "Mobility";
    case 52: return "CGM";
    case 53: return "Insulin pump";
    case 81: return "Outdoor sport";
    }
    return NULL;
}

/* The first Apple Continuity message type in Apple manufacturer data. */
static const char *bt_apple_type_str(uint8_t type)
{
    switch (type) {
    case 0x02: return "iBeacon";
    case 0x05: return "AirDrop";
    case 0x07: return "AirPods";
    case 0x09: return "AirPlay tgt";
    case 0x0A: return "AirPlay src";
    case 0x0B: return "Watch";
    case 0x0C: return "Handoff";
    case 0x0D: return "WiFi set";
    case 0x0E: return "Hotspot";
    case 0x0F: return "Nearby act";
    case 0x10: return "Nearby";
    case 0x12: return "Find My";
    }
    return NULL;
}

static bool bt_has_uuid(const bt_scan_result_t *dev, uint16_t uuid)
{
    return dev->service_uuid == uuid || dev->svc_data_uuid == uuid;
}

/* Beacon or tracker kind, from its advertising data, or NULL. */
static const char *bt_tag(const bt_scan_result_t *dev)
{
    const uint8_t *m = dev->mfr_data;
    if (dev->company_id == 0x004C && dev->mfr_data_len >= 23 &&
        m[0] == 0x02 && m[1] == 0x15)                  return "iBeacon";
    if (dev->company_id == 0x004C && dev->mfr_data_len >= 1 &&
        m[0] == 0x12)                                  return "Find My";
    if (dev->company_id != BT_COMPANY_NONE && dev->mfr_data_len >= 2 &&
        m[0] == 0xBE && m[1] == 0xAC)                  return "AltBeacon";
    if (bt_has_uuid(dev, 0xFEAA))                      return "Eddystone";
    if (bt_has_uuid(dev, 0xFEED) || bt_has_uuid(dev, 0xFEEC))
                                                       return "Tile";
    if (bt_has_uuid(dev, 0xFD5A))                      return "SmartTag";
    return NULL;
}

/* OUI vendor of a public address (Classic, or BLE public), else NULL. */
static const char *bt_vendor(const bt_scan_result_t *dev)
{
    bool pub = dev->type == BT_DEVTYPE_CLASSIC ||
               dev->addr_type == BT_ADDR_PUBLIC;
    if (!pub) return NULL;
    const uint8_t *a = dev->addr;   /* LSB first */
    return oui_vendor(a[5], a[4], a[3]);
}

/* Best short label for a device: its name, else its beacon kind, else its
 * vendor. */
static const char *bt_label(const bt_scan_result_t *dev)
{
    if (dev->name[0]) return dev->name;
    const char *t = bt_tag(dev);
    if (t) return t;
    t = bt_vendor(dev);
    return t ? t : "Unknown";
}

/* Device type in a Microsoft CDP beacon (Windows "Swift Pair"/CDP). */
static const char *bt_ms_device_str(uint8_t dev)
{
    switch (dev) {
    case 1:  return "Xbox One";
    case 6:  return "iPhone";
    case 7:  return "iPad";
    case 8:  return "Android";
    case 9:  return "Win desktop";
    case 11: return "Win phone";
    case 12: return "Linux";
    case 13: return "Win IoT";
    case 14: return "Surface Hub";
    case 15: return "Win laptop";
    case 16: return "Win tablet";
    }
    return NULL;
}

static int16_t be16s(const uint8_t *d) { return (int16_t)((uint16_t)d[0] << 8 | d[1]); }
static int16_t le16s(const uint8_t *d) { return (int16_t)((uint16_t)d[1] << 8 | d[0]); }

/* Format a fixed-point value with `div` 10 or 100 as "-12.34". */
static int fmt_fixed(char *buf, size_t len, int v, int div)
{
    const char *sign = v < 0 ? "-" : "";
    if (v < 0) v = -v;
    return snprintf(buf, len, div == 100 ? "%s%d.%02d" : "%s%d.%d",
                    sign, v / div, v % div);
}

/* Readings in Service Data, as one display line: Eddystone TLM, BTHome v2,
 * and the ATC / pvvx firmware for Xiaomi thermometers.  Returns false if the
 * data is none of these. */
static bool bt_svc_data_reading(const bt_scan_result_t *dev, char *buf, size_t len)
{
    const uint8_t *d = dev->svc_data;
    int n = dev->svc_data_len;
    char t[12];

    if (dev->svc_data_uuid == 0xFEAA && n >= 6 && d[0] == 0x20) {
        /* Eddystone TLM: battery mV, then temperature in 8.8 fixed point. */
        fmt_fixed(t, sizeof t, be16s(&d[4]) * 10 / 256, 10);
        snprintf(buf, len, "%umV %sC", (unsigned)((d[2] << 8) | d[3]), t);
        return true;
    }
    if (dev->svc_data_uuid == 0x181A && n == 13) {
        /* ATC1441: MAC, temp 0.1 C (BE), humidity %, battery %, mV (BE). */
        fmt_fixed(t, sizeof t, be16s(&d[6]), 10);
        snprintf(buf, len, "%sC %u%% B%u%%", t, d[8], d[9]);
        return true;
    }
    if (dev->svc_data_uuid == 0x181A && n >= 14) {
        /* pvvx: MAC, temp 0.01 C, humidity 0.01 %, mV, battery % (LE). */
        fmt_fixed(t, sizeof t, le16s(&d[6]), 100);
        snprintf(buf, len, "%sC %u%% B%u%%", t,
                 (unsigned)((d[9] << 8) | d[8]) / 100u, d[12]);
        return true;
    }
    if (dev->svc_data_uuid == 0xFCD2 && n >= 1) {
        /* BTHome v2: an info byte, then (object ID, value) pairs in ID
         * order.  Stops at the first ID it does not know the size of. */
        if (d[0] & 0x01u) { snprintf(buf, len, "(encrypted)"); return true; }
        size_t pos = 0;
        buf[0] = '\0';
        for (int i = 1; i < n && pos < len; ) {
            uint8_t id = d[i++];
            int w = 0;
            if (id == 0x00 && i + 1 <= n) {            /* packet id */
                i += 1;
            } else if (id == 0x01 && i + 1 <= n) {     /* battery % */
                w = snprintf(buf + pos, len - pos, "B%u%% ", d[i]); i += 1;
            } else if (id == 0x02 && i + 2 <= n) {     /* temp 0.01 C */
                fmt_fixed(t, sizeof t, le16s(&d[i]), 100);
                w = snprintf(buf + pos, len - pos, "%sC ", t); i += 2;
            } else if (id == 0x03 && i + 2 <= n) {     /* humidity 0.01 % */
                w = snprintf(buf + pos, len - pos, "%u%% ",
                             (unsigned)((d[i + 1] << 8) | d[i]) / 100u);
                i += 2;
            } else if (id == 0x2E && i + 1 <= n) {     /* humidity % */
                w = snprintf(buf + pos, len - pos, "%u%% ", d[i]); i += 1;
            } else if (id == 0x45 && i + 2 <= n) {     /* temp 0.1 C */
                fmt_fixed(t, sizeof t, le16s(&d[i]), 10);
                w = snprintf(buf + pos, len - pos, "%sC ", t); i += 2;
            } else {
                break;
            }
            if (w < 0 || (size_t)w >= len - pos) break;
            pos += (size_t)w;
        }
        return buf[0] != '\0';
    }
    return false;
}

/* Rough distance from path loss, log-distance model with exponent 2 (free
 * space; indoors it reads short).  `loss_1m` is the loss to 1 m in dB.
 * Returns decimetres, or -1 if out of range.  Integer only: no libm. */
static int bt_distance_dm(int loss_1m)
{
    /* 10^(k/20) * 1000 for k = 0..19: each 20 dB is a factor of 10. */
    static const uint16_t f[20] = {
        1000, 1122, 1259, 1413, 1585, 1778, 1995, 2239, 2512, 2818,
        3162, 3548, 3981, 4467, 5012, 5623, 6310, 7079, 7943, 8913 };
    if (loss_1m < -20) return 0;
    if (loss_1m >= 60) return -1;              /* over 1 km: not useful */
    if (loss_1m < 0)   return (int)f[loss_1m + 20] / 1000;
    int dm = f[loss_1m % 20] / 100;            /* 1 m = 10 dm */
    for (int k = loss_1m / 20; k > 0; k--) dm *= 10;
    return dm;
}

/* ---- co-presence pairs ---------------------------------------------------- */

/* What a device is likely to be in a pairing: a phone or computer (hub), or
 * something that connects to one (accessory: watch, earbuds, HID ...). */
typedef enum { ROLE_OTHER = 0, ROLE_HUB, ROLE_ACC } bt_role_t;

static bt_role_t bt_role(const bt_scan_result_t *dev)
{
    static const char *const acc_names[] = {
        "AirPods", "Buds", "Watch", "Pods", "Band", "Beats", "Headphone",
        "Headset", "Earbud", "WH-", "WF-", "Keyboard", "Mouse" };
    static const char *const hub_names[] = {
        "iPhone", "iPad", "MacBook", "iMac", "Pixel", "Galaxy", "Phone",
        "Laptop" };
    if (bt_tag(dev)) return ROLE_OTHER;     /* beacons and trackers */
    /* Accessory names first: "Galaxy Buds" is not a phone. */
    if (dev->name[0]) {
        for (int i = 0; i < (int)(sizeof acc_names / sizeof acc_names[0]); i++)
            if (strstr(dev->name, acc_names[i])) return ROLE_ACC;
        for (int i = 0; i < (int)(sizeof hub_names / sizeof hub_names[0]); i++)
            if (strstr(dev->name, hub_names[i])) return ROLE_HUB;
    }
    if (dev->class_of_device) {
        switch ((dev->class_of_device >> 8) & 0x1Fu) {
        case 1: case 2:         return ROLE_HUB;   /* computer, phone      */
        case 4: case 5: case 7:
        case 9:                 return ROLE_ACC;   /* audio, HID, wearable,
                                                    * health              */
        }
    }
    if (dev->appearance != BT_APPEARANCE_NONE) {
        switch (dev->appearance >> 6) {
        case 1: case 2:         return ROLE_HUB;
        case 3: case 13: case 15: case 17: case 18: case 33: case 37:
        case 41:                return ROLE_ACC;
        }
    }
    if (dev->company_id == 0x004C && dev->mfr_data_len >= 1) {
        if (dev->mfr_data[0] == 0x07 || dev->mfr_data[0] == 0x0B)
            return ROLE_ACC;                       /* AirPods, Watch       */
    }
    if (dev->company_id == 0x0006 && dev->mfr_data_len >= 2 &&
        dev->mfr_data[0] == 0x01) {                /* Microsoft CDP        */
        switch (dev->mfr_data[1] & 0x1Fu) {
        case 6: case 7: case 8: case 9: case 11: case 15: case 16:
            return ROLE_HUB;
        }
    }
    if (bt_has_uuid(dev, 0xFE2C)) return ROLE_ACC; /* Fast Pair accessory  */
    return ROLE_OTHER;
}

static const char *bt_role_str(bt_role_t r)
{
    return r == ROLE_HUB ? "Phone/PC" : r == ROLE_ACC ? "Accessory" : "Other";
}

/* A label that tells two unnamed devices apart: name, else its kind and the
 * last two address bytes. */
static void bt_pair_label(const bt_scan_result_t *dev, char *buf, size_t len)
{
    if (dev->name[0]) { snprintf(buf, len, "%s", dev->name); return; }
    const char *k = bt_tag(dev);
    if (!k && dev->company_id == 0x004C && dev->mfr_data_len >= 1)
        k = bt_apple_type_str(dev->mfr_data[0]);
    if (!k && dev->appearance != BT_APPEARANCE_NONE)
        k = bt_appearance_str(dev->appearance);
    if (!k) k = bt_vendor(dev);
    if (!k && dev->company_id != BT_COMPANY_NONE)
        k = bt_company_name(dev->company_id);
    if (!k) k = "?";
    snprintf(buf, len, "%.9s %02X%02X", k, dev->addr[1], dev->addr[0]);
}

static uint64_t isqrt64(uint64_t v)
{
    uint64_t r = 0u, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* Pearson correlation x 100 from running sums, or CORR_NONE when either
 * series varies less than `min_var` (n^2 x variance). */
static int pearson100(int64_t n, int64_t sx, int64_t sy, int64_t sxx,
                      int64_t syy, int64_t sxy, int64_t min_var)
{
    int64_t vx = n * sxx - sx * sx;
    int64_t vy = n * syy - sy * sy;
    if (vx < min_var || vy < min_var) return CORR_NONE;
    uint64_t d = isqrt64((uint64_t)vx * (uint64_t)vy);
    if (d == 0u) return CORR_NONE;
    return (int)((n * sxy - sx * sy) * 100 / (int64_t)d);
}

typedef struct {
    uint8_t a, b;      /* s_bt_cache slots                                  */
    int16_t score;     /* 0..100: the stronger of pres and rssi             */
    int16_t pres;      /* presence correlation x 100, or CORR_NONE          */
    int16_t rssi;      /* RSSI correlation x 100, or CORR_NONE              */
    int16_t together;  /* % of windows either was present that both were    */
    int16_t samples;   /* windows both were heard (RSSI pairs)              */
    bool    fit;       /* one phone/computer and one accessory              */
} corr_pair_t;

/* Score slots a and b over the recorded windows.  False if they were never
 * present together.  Caller holds s_bt_lock. */
static bool corr_pair(int a, int b, corr_pair_t *out)
{
    int n = s_corr_n, pa = 0, pb = 0, pab = 0, m = 0;
    int64_t sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int k = 0; k < n; k++) {
        int pos = (int)((s_corr_win - (uint32_t)k) % CORR_WINDOWS);
        int x = s_corr[a][pos], y = s_corr[b][pos];
        pa  += x != 0;
        pb  += y != 0;
        pab += x != 0 && y != 0;
        if (x < 0 && y < 0) {
            m++;
            sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        }
    }
    if (pab == 0) return false;
    out->a        = (uint8_t)a;
    out->b        = (uint8_t)b;
    /* Presence as 0/1 series: sum of squares equals the sum. */
    out->pres     = (int16_t)pearson100(n, pa, pb, pa, pb, pab, 1);
    out->rssi     = m >= CORR_MIN_SAMPLES
                  ? (int16_t)pearson100(m, sx, sy, sxx, syy, sxy, (int64_t)m * m)
                  : (int16_t)CORR_NONE;   /* min_var: a 1 dB spread at least */
    out->together = (int16_t)(pab * 100 / (pa + pb - pab));
    out->samples  = (int16_t)m;
    int s = out->pres > out->rssi ? out->pres : out->rssi;
    out->score    = (int16_t)(s < 0 ? 0 : s);
    bt_role_t ra = bt_role(&s_bt_cache[a]), rb = bt_role(&s_bt_cache[b]);
    out->fit      = (ra == ROLE_HUB && rb == ROLE_ACC) ||
                    (ra == ROLE_ACC && rb == ROLE_HUB);
    return true;
}

/* The Pairs list, best first, rebuilt every window.  Main thread only; its
 * slots are valid while s_bt_lock is held, until the next merge. */
static corr_pair_t s_pairs[CORR_MAX_PAIRS];
static int         s_pair_count = 0;

static void corr_build(void)
{
    s_pair_count = 0;
    if (s_corr_n < CORR_MIN_WINDOWS) return;
    for (int a = 0; a < s_bt_count; a++) {
        for (int b = a + 1; b < s_bt_count; b++) {
            corr_pair_t p;
            if (!corr_pair(a, b, &p) || p.score < CORR_MIN_SCORE) continue;
            /* Insertion into the sorted top list; a phone-accessory pair
             * wins a tie. */
            int at = s_pair_count;
            while (at > 0 && (s_pairs[at - 1].score < p.score ||
                              (s_pairs[at - 1].score == p.score &&
                               p.fit && !s_pairs[at - 1].fit)))
                at--;
            if (at >= CORR_MAX_PAIRS) continue;
            int last = s_pair_count < CORR_MAX_PAIRS ? s_pair_count
                                                     : CORR_MAX_PAIRS - 1;
            for (int k = last; k > at; k--) s_pairs[k] = s_pairs[k - 1];
            s_pairs[at] = p;
            if (s_pair_count < CORR_MAX_PAIRS) s_pair_count++;
        }
    }
}

/* Cache slot holding `addr`, or -1.  Caller holds s_bt_lock. */
static int bt_find(const uint8_t *addr)
{
    for (int i = 0; i < s_bt_count; i++)
        if (memcmp(s_bt_cache[i].addr, addr, BT_ADDR_LEN) == 0) return i;
    return -1;
}

/* ---- WiFi helpers --------------------------------------------------------- */

/* Map RSSI to a display color per the signal-quality table. */
/* Distance estimate in decimetres for a cached device, or -1: from an
 * iBeacon's measured power at 1 m if it has one, else from TX power at
 * 0 m (about 41 dB lost in the first metre). */
static int bt_dist(const bt_scan_result_t *dev)
{
    bool ibeacon = dev->company_id == 0x004C && dev->mfr_data_len >= 23 &&
                   dev->mfr_data[0] == 0x02 && dev->mfr_data[1] == 0x15;
    if (ibeacon)
        return bt_distance_dm((int8_t)dev->mfr_data[22] - dev->rssi);
    if (dev->tx_power != BT_TX_POWER_UNKNOWN)
        return bt_distance_dm(dev->tx_power - dev->rssi - 41);
    return -1;
}

static uint8_t rssi_color(int16_t rssi)
{
    if (rssi > -67) return COLOR_GREEN;
    if (rssi > -70) return COLOR_YELLOW;
    if (rssi > -80) return COLOR_ORANGE;
    return COLOR_RED;
}

/* auth_mode is a WIFI_AUTH_* bitmask.  WPA and RSN networks also set the
 * WEP (privacy) bit, so it means WEP only when neither IE is present.  RSN
 * does not say whether the AP runs WPA2 or WPA3. */
static const char *auth_label(uint8_t auth_mode)
{
    bool wpa = auth_mode & WIFI_AUTH_WPA;
    bool rsn = auth_mode & WIFI_AUTH_RSN;
    if (wpa && rsn)                  return "WPA/WPA2";
    if (rsn)                         return "WPA2/WPA3";
    if (wpa)                         return "WPA";
    if (auth_mode & WIFI_AUTH_WEP)   return "WEP";
    return "Open";
}

/* Weak security: anything without RSN (WPA2/WPA3) — open, WEP or WPA1. */
static bool auth_weak(uint8_t auth_mode)
{
    return (auth_mode & WIFI_AUTH_RSN) == 0u;
}

static uint8_t auth_color(uint8_t auth_mode)
{
    if (auth_mode & WIFI_AUTH_RSN) return COLOR_GREY;
    if (auth_mode & WIFI_AUTH_WPA) return COLOR_ORANGE;
    return COLOR_RED;              /* open or WEP */
}

/* Vendor of a MAC (first three octets in printed order): the OUI vendor,
 * "(random)" for a locally administered address, or NULL if unknown. */
static const char *mac_vendor(uint8_t o0, uint8_t o1, uint8_t o2)
{
    if (oui_is_local(o0)) return "(random)";
    return oui_vendor(o0, o1, o2);
}

/* Center frequency of a 2.4 GHz channel (the CYW43439 has no 5 GHz radio). */
static int chan_mhz(int ch)
{
    return ch == 14 ? 2484 : 2407 + 5 * ch;
}

/* Two scan results are the same network if they share an SSID; hidden
 * networks (empty SSID) have no name to share, so each BSSID is its own. */
static bool same_net(const wifi_scan_result_t *a, const wifi_scan_result_t *b)
{
    if (a->ssid[0] == '\0' || b->ssid[0] == '\0')
        return a->ssid[0] == b->ssid[0] &&
               memcmp(a->bssid, b->bssid, sizeof(a->bssid)) == 0;
    return strcmp(a->ssid, b->ssid) == 0;
}

static const char *wifi_name(const wifi_scan_result_t *net)
{
    return net->ssid[0] ? net->ssid : "<hidden>";
}

/*
 * Build a deduplicated index list from raw scan results.
 * Invalid readings (rssi >= 0) are skipped.  When the same network (see
 * same_net) appears more than once, the entry with the stronger RSSI is
 * kept, and out_aps[] counts how many BSSIDs carried it.
 * Returns the number of unique entries written to out_idx[] / out_aps[].
 */
static int build_dedup(const wifi_scan_result_t *results, int count,
                       int *out_idx, int *out_aps, int max_out)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        /* The CYW43 firmware occasionally reports rssi 0 — not a real
         * reading, and it would beat every genuine one in the max below. */
        if (results[i].rssi >= 0) continue;
        int dup = -1;
        for (int j = 0; j < n; j++) {
            if (same_net(&results[i], &results[out_idx[j]])) {
                dup = j;
                break;
            }
        }
        if (dup >= 0) {
            out_aps[dup]++;
            if (results[i].rssi > results[out_idx[dup]].rssi)
                out_idx[dup] = i;
        } else if (n < max_out) {
            out_aps[n]   = 1;
            out_idx[n++] = i;
        }
    }
    return n;
}

static void cache_put(int slot, const wifi_scan_result_t *r, int aps,
                      uint32_t win)
{
    s_wifi_cache[slot]      = *r;
    s_wifi_aps[slot]        = aps;
    s_wifi_chan_count[slot] = 1;
    s_wifi_chans[slot][0]   = r->channel;
    s_wifi_seen[slot]       = true;
    s_wifi_last_win[slot]   = win;
}

/*
 * Merge a deduplicated scan batch (raw[idx[0..count-1]]) from window `win`
 * into s_wifi_cache.  Networks already cached take the whole latest reading
 * (from the strongest AP, so BSSID, RSSI and the rest stay consistent) and
 * its channel is added to the list.  New networks are appended; when the
 * cache is full, one replaces the entry heard longest ago, if that was more
 * than WIFI_EVICT_WINDOWS windows back.  s_wifi_seen[] is rebuilt to mark
 * exactly the entries in this batch.
 */
static void merge_into_cache(const wifi_scan_result_t *raw,
                             const int *idx, const int *aps, int count,
                             uint32_t win)
{
    bool placed[WIFI_MAX_SCAN_RESULTS] = {false};
    memset(s_wifi_seen, 0, sizeof(s_wifi_seen));

    /* Pass 1: update networks already cached. */
    for (int i = 0; i < count; i++) {
        const wifi_scan_result_t *r = &raw[idx[i]];
        for (int j = 0; j < s_wifi_ucount; j++) {
            if (!same_net(r, &s_wifi_cache[j])) continue;
            s_wifi_cache[j]    = *r;
            s_wifi_aps[j]      = aps[i];
            s_wifi_seen[j]     = true;
            s_wifi_last_win[j] = win;
            /* Append channel to list if not already present */
            bool ch_seen = false;
            for (int k = 0; k < s_wifi_chan_count[j]; k++) {
                if (s_wifi_chans[j][k] == r->channel) { ch_seen = true; break; }
            }
            if (!ch_seen && s_wifi_chan_count[j] < (int)MAX_CHANS_PER_SSID)
                s_wifi_chans[j][s_wifi_chan_count[j]++] = r->channel;
            placed[i] = true;
            break;
        }
    }

    /* Pass 2: add new ones, evicting stale entries when full. */
    for (int i = 0; i < count; i++) {
        if (placed[i]) continue;
        const wifi_scan_result_t *r = &raw[idx[i]];
        if (s_wifi_ucount < WIFI_MAX_SCAN_RESULTS) {
            cache_put(s_wifi_ucount, r, aps[i], win);
            s_wifi_ucount++;        /* increment after full write */
            continue;
        }
        int old = -1;
        for (int j = 0; j < s_wifi_ucount; j++) {
            if (win - s_wifi_last_win[j] <= WIFI_EVICT_WINDOWS) continue;
            if (old < 0 || s_wifi_last_win[j] < s_wifi_last_win[old]) old = j;
        }
        if (old >= 0) cache_put(old, r, aps[i], win);
    }
}

/* Track every BSSID of the s_ap_key network from one raw window.  Caller
 * holds s_wifi_lock. */
static void ap_track(const wifi_scan_result_t *items, int count, uint32_t win)
{
    if (!s_ap_key_set) return;
    for (int i = 0; i < count; i++) {
        const wifi_scan_result_t *r = &items[i];
        if (r->rssi >= 0 || !same_net(r, &s_ap_key)) continue;
        int slot = -1;
        for (int j = 0; j < s_ap_count; j++)
            if (memcmp(s_ap_list[j].bssid, r->bssid, 6) == 0) { slot = j; break; }
        if (slot < 0) {
            if (s_ap_count < MAX_APS) {
                slot = s_ap_count++;
            } else {
                slot = 0;
                for (int j = 1; j < s_ap_count; j++)
                    if (s_ap_list[j].last_win < s_ap_list[slot].last_win) slot = j;
            }
            memcpy(s_ap_list[slot].bssid, r->bssid, 6);
        }
        s_ap_list[slot].channel  = r->channel;
        s_ap_list[slot].rssi     = (int8_t)r->rssi;
        s_ap_list[slot].last_win = win;
    }
    /* Drop APs not heard within the hold. */
    int n = 0;
    for (int j = 0; j < s_ap_count; j++)
        if (win - s_ap_list[j].last_win < AP_HOLD_WINDOWS)
            s_ap_list[n++] = s_ap_list[j];
    s_ap_count = n;
}

/* Record one raw scan window in the survey ring.  Caller holds s_wifi_lock. */
static void survey_window(const wifi_scan_result_t *items, int count)
{
    chan_win_t *w = s_survey[s_survey_head];
    int nsum[SURVEY_CHANS] = {0};
    int ncnt[SURVEY_CHANS] = {0};

    memset(w, 0, sizeof(s_survey[0]));
    for (int i = 0; i < count; i++) {
        const wifi_scan_result_t *r = &items[i];
        if (r->rssi >= 0 || r->channel < 1 || r->channel > SURVEY_CHANS)
            continue;
        chan_win_t *c = &w[r->channel - 1];
        c->aps++;
        if (r->rssi > SURVEY_STRONG) c->strong++;
        if (c->best_rssi == 0 || r->rssi > c->best_rssi)
            c->best_rssi = (int8_t)r->rssi;
        if (r->noise != 0) {
            nsum[r->channel - 1] += r->noise;
            ncnt[r->channel - 1]++;
        }
    }
    for (int ch = 0; ch < SURVEY_CHANS; ch++)
        if (ncnt[ch] > 0) w[ch].noise = (int8_t)(nsum[ch] / ncnt[ch]);

    s_survey_head = (s_survey_head + 1) % SURVEY_WINDOWS;
}

typedef struct {
    int aps;        /* most BSSIDs heard in one window                       */
    int strong;     /* most strong BSSIDs heard in one window                */
    int best_rssi;  /* strongest RSSI over the ring, 0 = none                */
    int noise;      /* mean noise floor over the ring, 0 = unknown           */
} chan_stat_t;

/* Reduce the survey ring to one channel (index 0..SURVEY_CHANS-1). */
static chan_stat_t survey_chan(int ch)
{
    chan_stat_t s = {0, 0, 0, 0};
    int nsum = 0, ncnt = 0;
    for (int w = 0; w < SURVEY_WINDOWS; w++) {
        const chan_win_t *c = &s_survey[w][ch];
        if (c->aps > s.aps)       s.aps    = c->aps;
        if (c->strong > s.strong) s.strong = c->strong;
        if (c->best_rssi != 0 && (s.best_rssi == 0 || c->best_rssi > s.best_rssi))
            s.best_rssi = c->best_rssi;
        if (c->noise != 0) { nsum += c->noise; ncnt++; }
    }
    if (ncnt > 0) s.noise = nsum / ncnt;
    return s;
}

/* Congestion load on a channel: every AP within 4 channels (20 MHz wide at
 * 5 MHz spacing) adds 5 - distance, doubled for strong APs. */
static int survey_load(const chan_stat_t *st, int ch)
{
    int load = 0;
    for (int o = 0; o < SURVEY_CHANS; o++) {
        int d = abs(o - ch);
        if (d >= 5) continue;
        load += (5 - d) * (st[o].aps + st[o].strong);
    }
    return load;
}

/* Load on channel `ch` from survey window `w` alone (for the history). */
static int window_load(int w, int ch)
{
    int load = 0;
    for (int o = 0; o < SURVEY_CHANS; o++) {
        int d = abs(o - ch);
        if (d >= 5) continue;
        load += (5 - d) * (s_survey[w][o].aps + s_survey[w][o].strong);
    }
    return load;
}

/* Add the newest survey window's figures for `ch` to the history. */
static void chan_hist_push(int ch)
{
    int w    = (s_survey_head + SURVEY_WINDOWS - 1) % SURVEY_WINDOWS;
    int load = window_load(w, ch);
    s_chan_hist_load[s_chan_hist_head]  = (uint8_t)(load > 254 ? 255 : load + 1);
    s_chan_hist_noise[s_chan_hist_head] = s_survey[w][ch].noise;
    s_chan_hist_head = (s_chan_hist_head + 1) % (int)DISP_WIDTH;
}

static void chan_hist_reset(void)
{
    memset(s_chan_hist_load, 0, sizeof(s_chan_hist_load));
    memset(s_chan_hist_noise, 0, sizeof(s_chan_hist_noise));
    s_chan_hist_head = 0;
}

static uint8_t load_color(int load)
{
    if (load < 10) return COLOR_GREEN;
    if (load < 25) return COLOR_YELLOW;
    if (load < 45) return COLOR_ORANGE;
    return COLOR_RED;
}

/* A quiet 2.4 GHz channel reads about -92 dBm; a raised floor means
 * something (WiFi or not) is putting energy on it. */
static uint8_t noise_color(int noise)
{
    if (noise == 0)   return COLOR_GREY;
    if (noise >= -75) return COLOR_RED;
    if (noise >= -82) return COLOR_ORANGE;
    if (noise >= -88) return COLOR_YELLOW;
    return COLOR_GREEN;
}

/* ---- time ----------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    uint32_t t = 0u;
    dev_ioctl(DEV_TIMER, IOCTL_TIMER_GET_TICK, &t);   /* 1 ms scheduler tick */
    return t;
}

/* ---- watch list ------------------------------------------------------------ *
 * Networks and devices to report arriving and leaving.  Kept in WATCH_FILE
 * so it survives a reboot.  Every scan window, on any screen, the worker
 * marks each entry of that radio heard or missed; an entry missed for both
 * WATCH_GONE_MS and its radio's GONE windows has left.  The window counts
 * matter on the Watch screen, which alternates the radios.
 * Guarded by s_watch_lock (taken after a cache lock, never before).        */
#define WATCH_MAX           8
#define WATCH_EVENTS        6
#define WATCH_GONE_MS       60000u
#define WATCH_GONE_WIN_WIFI 10u
#define WATCH_GONE_WIN_BT   30u
#define WATCH_FILE          "watch.cfg"

enum { W_UNSEEN = 0, W_HERE, W_GONE };

typedef struct {
    char     kind;          /* 'W' WiFi by SSID, 'H' hidden WiFi by BSSID,
                             * 'B' Bluetooth by address                    */
    uint8_t  state;         /* W_UNSEEN / W_HERE / W_GONE                  */
    uint16_t missed;        /* windows of its radio since last heard       */
    uint8_t  mac[6];        /* BSSID ('H') or BT address, LSB first ('B')  */
    char     name[33];      /* SSID, or the BT label when added            */
    uint32_t last_ms;
} watch_t;

typedef struct {
    char     name[20];
    bool     arrived;
    uint32_t ms;
} watch_event_t;

static kmutex_t      s_watch_lock;
static watch_t       s_watch[WATCH_MAX];
static int           s_watch_count = 0;
static watch_event_t s_watch_ev[WATCH_EVENTS];   /* ring, newest at head-1 */
static int           s_watch_ev_head = 0;
static int           s_watch_ev_count = 0;
/* Bumped by the worker once per event.  Lock-free: a single aligned word
 * with one writer; main compares it to the last value it handled.        */
static volatile uint32_t s_watch_seq = 0u;
static bool          s_watch_dirty = false;   /* main only: needs saving */

static bool watch_match_wifi(const watch_t *w, const wifi_scan_result_t *r)
{
    if (w->kind == 'W') return r->ssid[0] && strcmp(w->name, r->ssid) == 0;
    if (w->kind == 'H') return !r->ssid[0] && memcmp(w->mac, r->bssid, 6) == 0;
    return false;
}

/* Index of the entry for a network / device, or -1.  Caller holds the lock. */
static int watch_find_wifi(const wifi_scan_result_t *net)
{
    for (int i = 0; i < s_watch_count; i++)
        if (watch_match_wifi(&s_watch[i], net)) return i;
    return -1;
}

static int watch_find_bt(const uint8_t *addr)
{
    for (int i = 0; i < s_watch_count; i++)
        if (s_watch[i].kind == 'B' && memcmp(s_watch[i].mac, addr, 6) == 0)
            return i;
    return -1;
}

static bool watch_has_wifi(const wifi_scan_result_t *net)
{
    kmutex_lock(&s_watch_lock);
    bool r = watch_find_wifi(net) >= 0;
    kmutex_unlock(&s_watch_lock);
    return r;
}

static bool watch_has_bt(const uint8_t *addr)
{
    kmutex_lock(&s_watch_lock);
    bool r = watch_find_bt(addr) >= 0;
    kmutex_unlock(&s_watch_lock);
    return r;
}

static void watch_remove_locked(int i)
{
    for (int j = i; j < s_watch_count - 1; j++) s_watch[j] = s_watch[j + 1];
    s_watch_count--;
    s_watch_dirty = true;
}

static watch_t *watch_add_locked(char kind, const char *name, const uint8_t *mac)
{
    if (s_watch_count >= WATCH_MAX) return NULL;
    watch_t *w = &s_watch[s_watch_count++];
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    strncpy(w->name, name, sizeof(w->name) - 1u);
    if (mac) memcpy(w->mac, mac, 6);
    s_watch_dirty = true;
    return w;
}

/* Add the network if it is not watched, else remove it.  Main thread. */
static void watch_toggle_wifi(const wifi_scan_result_t *net)
{
    kmutex_lock(&s_watch_lock);
    int i = watch_find_wifi(net);
    if (i >= 0)               watch_remove_locked(i);
    else if (net->ssid[0])    watch_add_locked('W', net->ssid, NULL);
    else                      watch_add_locked('H', "<hidden>", net->bssid);
    kmutex_unlock(&s_watch_lock);
}

static void watch_toggle_bt(const bt_scan_result_t *dev)
{
    kmutex_lock(&s_watch_lock);
    int i = watch_find_bt(dev->addr);
    if (i >= 0) watch_remove_locked(i);
    else        watch_add_locked('B', bt_label(dev), dev->addr);
    kmutex_unlock(&s_watch_lock);
}

/* Record an arrival or departure.  Caller (the worker) holds the lock. */
static void watch_event(const watch_t *w, bool arrived, uint32_t now)
{
    watch_event_t *e = &s_watch_ev[s_watch_ev_head];
    strncpy(e->name, w->name, sizeof(e->name) - 1u);
    e->name[sizeof(e->name) - 1u] = '\0';
    e->arrived = arrived;
    e->ms      = now;
    s_watch_ev_head = (s_watch_ev_head + 1) % WATCH_EVENTS;
    if (s_watch_ev_count < WATCH_EVENTS) s_watch_ev_count++;
    s_watch_seq++;
}

/* One window's verdict for an entry: heard or missed. */
static void watch_update(watch_t *w, bool heard, uint32_t now, uint32_t gone_win)
{
    if (heard) {
        if (w->state != W_HERE) {
            w->state = W_HERE;
            watch_event(w, true, now);
        }
        w->missed  = 0;
        w->last_ms = now;
        return;
    }
    if (w->missed < 0xFFFFu) w->missed++;
    if (w->state == W_HERE && w->missed >= gone_win &&
        now - w->last_ms >= WATCH_GONE_MS) {
        w->state = W_GONE;
        watch_event(w, false, now);
    }
}

static void watch_wifi_window(const wifi_scan_result_t *items, int count)
{
    uint32_t now = now_ms();
    kmutex_lock(&s_watch_lock);
    for (int i = 0; i < s_watch_count; i++) {
        watch_t *w = &s_watch[i];
        if (w->kind == 'B') continue;
        bool heard = false;
        for (int k = 0; k < count && !heard; k++)
            heard = items[k].rssi < 0 && watch_match_wifi(w, &items[k]);
        watch_update(w, heard, now, WATCH_GONE_WIN_WIFI);
    }
    kmutex_unlock(&s_watch_lock);
}

static void watch_bt_window(const bt_scan_result_t *items, int count)
{
    uint32_t now = now_ms();
    kmutex_lock(&s_watch_lock);
    for (int i = 0; i < s_watch_count; i++) {
        watch_t *w = &s_watch[i];
        if (w->kind != 'B') continue;
        bool heard = false;
        for (int k = 0; k < count && !heard; k++)
            heard = memcmp(items[k].addr, w->mac, 6) == 0;
        watch_update(w, heard, now, WATCH_GONE_WIN_BT);
    }
    kmutex_unlock(&s_watch_lock);
}

/* File format, one entry per line: <kind> <12 hex digits> <name>.  The
 * address is in stored order (BT: LSB first). */
static char s_file_buf[512];   /* watch file I/O; main thread only */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void watch_load(void)
{
    int fd = fs_open(WATCH_FILE, VFS_O_RDONLY);
    if (fd < 0) return;
    int n = fs_read(fd, (uint8_t *)s_file_buf, sizeof(s_file_buf) - 1u);
    fs_close(fd);
    if (n <= 0) return;
    s_file_buf[n] = '\0';

    kmutex_lock(&s_watch_lock);
    char *p = s_file_buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        char kind = p[0];
        if ((kind == 'W' || kind == 'H' || kind == 'B') && strlen(p) >= 15 &&
            p[1] == ' ' && p[14] == ' ') {
            uint8_t mac[6];
            bool ok = true;
            for (int i = 0; i < 6 && ok; i++) {
                int hi = hexval(p[2 + 2 * i]), lo = hexval(p[3 + 2 * i]);
                ok = hi >= 0 && lo >= 0;
                mac[i] = (uint8_t)(hi << 4 | lo);
            }
            if (ok) watch_add_locked(kind, p + 15, mac);
        }
        if (!eol) break;
        p = eol + 1;
    }
    s_watch_dirty = false;
    kmutex_unlock(&s_watch_lock);
}

/* Write the list to flash.  False if the file system refused (busy). */
static bool watch_save(void)
{
    int len = 0;
    kmutex_lock(&s_watch_lock);
    for (int i = 0; i < s_watch_count; i++) {
        const watch_t *w = &s_watch[i];
        const uint8_t *m = w->mac;
        len += snprintf(s_file_buf + len, sizeof(s_file_buf) - (size_t)len,
                        "%c %02X%02X%02X%02X%02X%02X %s\n", w->kind,
                        m[0], m[1], m[2], m[3], m[4], m[5], w->name);
        if (len >= (int)sizeof(s_file_buf)) { len = (int)sizeof(s_file_buf) - 1; break; }
    }
    kmutex_unlock(&s_watch_lock);

    if (len == 0) {
        fs_delete(WATCH_FILE);
        return true;
    }
    int fd = fs_open(WATCH_FILE, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) return false;
    fs_write(fd, (const uint8_t *)s_file_buf, (uint32_t)len);
    fs_close(fd);
    return true;
}

/* ---- survey log ------------------------------------------------------------ *
 * Every network and device heard this session, kept in RAM by the worker
 * and written to LOG_WIFI_FILE / LOG_BT_FILE as CSV every LOG_SAVE_MS and
 * on leaving a scanning screen, so `cat wifi.csv` in the shell shows the
 * last session even after a reboot.  When a table is full, the entry heard
 * longest ago makes room.  Times are seconds since boot.  Guarded by
 * s_log_lock (never held with another lock).                               */
#define LOG_MAX_WIFI   24
#define LOG_MAX_BT     24
#define LOG_SAVE_MS    300000u
#define LOG_WIFI_FILE  "wifi.csv"
#define LOG_BT_FILE    "bt.csv"

typedef struct {
    uint8_t  bssid[6];
    char     ssid[25];
    uint8_t  channel;
    uint8_t  auth;
    int8_t   best;
    uint32_t first_s, last_s;
} wlog_t;

typedef struct {
    uint8_t     addr[6];      /* LSB first */
    char        name[21];
    uint8_t     type;
    int8_t      best;         /* BT_RSSI_UNKNOWN until a valid reading */
    const char *tag;          /* bt_tag() once seen, else NULL */
    uint32_t    first_s, last_s;
} blog_t;

static kmutex_t s_log_lock;
static wlog_t   s_wlog[LOG_MAX_WIFI];
static blog_t   s_blog[LOG_MAX_BT];
static int      s_wlog_n = 0, s_blog_n = 0;
static bool     s_log_dirty = false;

static void log_wifi_window(const wifi_scan_result_t *items, int count)
{
    uint32_t now = now_ms() / 1000u;
    kmutex_lock(&s_log_lock);
    for (int i = 0; i < count; i++) {
        const wifi_scan_result_t *r = &items[i];
        if (r->rssi >= 0) continue;
        int slot = -1;
        for (int j = 0; j < s_wlog_n; j++)
            if (memcmp(s_wlog[j].bssid, r->bssid, 6) == 0) { slot = j; break; }
        if (slot < 0) {
            if (s_wlog_n < LOG_MAX_WIFI) {
                slot = s_wlog_n++;
            } else {
                slot = 0;
                for (int j = 1; j < s_wlog_n; j++)
                    if (s_wlog[j].last_s < s_wlog[slot].last_s) slot = j;
            }
            memset(&s_wlog[slot], 0, sizeof(s_wlog[slot]));
            memcpy(s_wlog[slot].bssid, r->bssid, 6);
            s_wlog[slot].best    = (int8_t)r->rssi;
            s_wlog[slot].first_s = now;
        }
        wlog_t *e = &s_wlog[slot];
        strncpy(e->ssid, r->ssid, sizeof(e->ssid) - 1u);
        e->channel = r->channel;
        e->auth    = r->auth_mode;
        if (r->rssi > e->best) e->best = (int8_t)r->rssi;
        e->last_s  = now;
    }
    if (count > 0) s_log_dirty = true;
    kmutex_unlock(&s_log_lock);
}

static void log_bt_window(const bt_scan_result_t *items, int count)
{
    uint32_t now = now_ms() / 1000u;
    kmutex_lock(&s_log_lock);
    for (int i = 0; i < count; i++) {
        const bt_scan_result_t *r = &items[i];
        int slot = -1;
        for (int j = 0; j < s_blog_n; j++)
            if (memcmp(s_blog[j].addr, r->addr, 6) == 0) { slot = j; break; }
        if (slot < 0) {
            if (s_blog_n < LOG_MAX_BT) {
                slot = s_blog_n++;
            } else {
                slot = 0;
                for (int j = 1; j < s_blog_n; j++)
                    if (s_blog[j].last_s < s_blog[slot].last_s) slot = j;
            }
            memset(&s_blog[slot], 0, sizeof(s_blog[slot]));
            memcpy(s_blog[slot].addr, r->addr, 6);
            s_blog[slot].best    = BT_RSSI_UNKNOWN;
            s_blog[slot].first_s = now;
        }
        blog_t *e = &s_blog[slot];
        if (r->name[0]) strncpy(e->name, r->name, sizeof(e->name) - 1u);
        e->type = (uint8_t)r->type;
        if (r->rssi != BT_RSSI_UNKNOWN &&
            (e->best == BT_RSSI_UNKNOWN || r->rssi > e->best))
            e->best = r->rssi;
        const char *t = bt_tag(r);
        if (t) e->tag = t;
        e->last_s = now;
    }
    if (count > 0) s_log_dirty = true;
    kmutex_unlock(&s_log_lock);
}

/* CSV fields: drop commas and quotes from names rather than escape them. */
static void csv_clean(char *dst, const char *src, size_t len)
{
    size_t n = 0;
    for (; *src && n + 1 < len; src++)
        if (*src != ',' && *src != '"' && *src >= ' ') dst[n++] = *src;
    dst[n] = '\0';
}

static bool log_write_wifi(void)
{
    static char line[112];
    int fd = fs_open(LOG_WIFI_FILE, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) return false;
    static const char hdr[] = "bssid,ssid,ch,security,best_dbm,vendor,first_s,last_s\n";
    fs_write(fd, (const uint8_t *)hdr, sizeof(hdr) - 1u);
    for (int i = 0; i < s_wlog_n; i++) {
        const wlog_t *e = &s_wlog[i];
        const uint8_t *b = e->bssid;
        char ssid[25];
        csv_clean(ssid, e->ssid, sizeof(ssid));
        const char *v = mac_vendor(b[0], b[1], b[2]);
        int n = snprintf(line, sizeof(line),
                         "%02X:%02X:%02X:%02X:%02X:%02X,%s,%u,%s,%d,%s,%lu,%lu\n",
                         b[0], b[1], b[2], b[3], b[4], b[5], ssid,
                         (unsigned)e->channel, auth_label(e->auth), (int)e->best,
                         v ? v : "", (unsigned long)e->first_s,
                         (unsigned long)e->last_s);
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        fs_write(fd, (const uint8_t *)line, (uint32_t)n);
    }
    fs_close(fd);
    return true;
}

static bool log_write_bt(void)
{
    static char line[112];
    int fd = fs_open(LOG_BT_FILE, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) return false;
    static const char hdr[] = "addr,name,type,best_dbm,kind,first_s,last_s\n";
    fs_write(fd, (const uint8_t *)hdr, sizeof(hdr) - 1u);
    for (int i = 0; i < s_blog_n; i++) {
        const blog_t *e = &s_blog[i];
        const uint8_t *a = e->addr;
        char name[21];
        csv_clean(name, e->name, sizeof(name));
        int n = snprintf(line, sizeof(line),
                         "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%d,%s,%lu,%lu\n",
                         a[5], a[4], a[3], a[2], a[1], a[0], name,
                         e->type == BT_DEVTYPE_BLE ? "BLE" : "Classic",
                         e->best == BT_RSSI_UNKNOWN ? 0 : (int)e->best,
                         e->tag ? e->tag : "", (unsigned long)e->first_s,
                         (unsigned long)e->last_s);
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        fs_write(fd, (const uint8_t *)line, (uint32_t)n);
    }
    fs_close(fd);
    return true;
}

/* Write both logs if anything changed.  Main thread. */
static void log_save(void)
{
    kmutex_lock(&s_log_lock);
    if (s_log_dirty) {
        bool ok = true;
        if (s_wlog_n > 0) ok = log_write_wifi() && ok;
        if (s_blog_n > 0) ok = log_write_bt() && ok;
        if (ok) s_log_dirty = false;
    }
    kmutex_unlock(&s_log_lock);
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
    int y = (int)TITLE_H + row_in_view * (int)DETAIL_ROW_H +
            ((int)DETAIL_ROW_H - 16) / 2;
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

/* ---- menus ---------------------------------------------------------------- *
 * Home and the WiFi / Bluetooth submenus.  An item opens a screen; for the
 * BT list, bt_filter picks all devices or only beacons and trackers.        */
typedef struct {
    const char *label;
    screen_t    target;
    bool        bt_filter;
} menu_item_t;

typedef struct {
    const char        *title;
    const menu_item_t *items;
    int                count;
} menu_t;

static const menu_item_t s_home_items[] = {
    { "WiFi",      SCR_WIFI_MENU, false },
    { "Bluetooth", SCR_BT_MENU,   false },
    { "Watch",     SCR_WATCH,     false },
    { "About",     SCR_ABOUT,     false },
};
static const menu_item_t s_wifi_items[] = {
    { "Scan",      SCR_WIFI,      false },
    { "Channels",  SCR_CHANNELS,  false },
};
static const menu_item_t s_bt_items[] = {
    { "Scan",      SCR_BT,        false },
    { "Beacons",   SCR_BT,        true  },
    { "Pairs",     SCR_BT_PAIRS,  false },
};
#define ITEMS(a) (a), (int)(sizeof(a) / sizeof((a)[0]))
static const menu_t s_menus[MENU_COUNT] = {
    [MENU_HOME] = { "NetMon V" NETMON_VERSION, ITEMS(s_home_items) },
    [MENU_WIFI] = { "WiFi",                    ITEMS(s_wifi_items) },
    [MENU_BT]   = { "Bluetooth",               ITEMS(s_bt_items)   },
};
#undef ITEMS

/* Menu shown by a screen, or -1 if it is not a menu. */
static int screen_menu(screen_t s)
{
    switch (s) {
    case SCR_HOME:      return MENU_HOME;
    case SCR_WIFI_MENU: return MENU_WIFI;
    case SCR_BT_MENU:   return MENU_BT;
    default:            return -1;
    }
}

/* Where Y goes from a screen (detail panels handle Y themselves). */
static screen_t screen_parent(screen_t s)
{
    switch (s) {
    case SCR_WIFI: case SCR_CHANNELS: return SCR_WIFI_MENU;
    case SCR_BT: case SCR_BT_PAIRS:   return SCR_BT_MENU;
    default:                          return SCR_HOME;
    }
}

static void render_menu(ui_state_t *st)
{
    const menu_t *m = &s_menus[screen_menu(st->screen)];
    draw_title(m->title);
    clear_content();
    for (int i = 0; i < m->count; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        draw_row(view, m->items[i].label, COLOR_GREY, i == st->sel);
    }
}

static void render_wifi(ui_state_t *st)
{
    draw_title("WiFi Networks");
    clear_content();

    if (s_wifi_ucount == 0) {
        draw_message(s_scanning_wifi ? "Scanning..." : "No networks found");
        return;
    }

    for (int i = 0; i < s_wifi_ucount; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const wifi_scan_result_t *net = &s_wifi_cache[i];
        char line[20];
        /* '!' between name and RSSI flags weak security (no WPA2/WPA3). */
        snprintf(line, sizeof(line), "%-11.11s%c%4d", wifi_name(net),
                 auth_weak(net->auth_mode) ? '!' : ' ', (int)net->rssi);
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

        /* Map RSSI (-100..-30 dBm) → bar height 1..GRAPH_H px. */
        int str = ((int)rv + 100) * (int)GRAPH_H / 70;
        if (str < 1)             str = 1;
        if (str > (int)GRAPH_H)  str = (int)GRAPH_H;

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

/* "Lo Hi Av" RSSI over a graph history (0 = not heard).  False if empty. */
static bool rssi_range(const int8_t *history, char *buf, size_t len)
{
    int lo = 0, hi = -128, sum = 0, cnt = 0;
    for (int i = 0; i < (int)DISP_WIDTH; i++) {
        int v = history[i];
        if (v == 0) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
        cnt++;
    }
    if (cnt == 0) return false;
    snprintf(buf, len, "Lo%d Hi%d Av%d", lo, hi, sum / cnt);
    return true;
}

/* Small arrows at the right edge of the detail rows: more above / below. */
static void draw_scroll_marks(bool up, bool down)
{
    int x = (int)DISP_WIDTH - 12;
    for (int r = 0; r < 6; r++) {
        if (up) {
            int y = (int)TITLE_H + 4 + r;
            disp_line_arg_t l = {
                .x0 = (uint16_t)(x + 5 - r), .y0 = (uint16_t)y,
                .x1 = (uint16_t)(x + 5 + r), .y1 = (uint16_t)y,
                .color = COLOR_RED, ._pad = 0
            };
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_LINE, &l);
        }
        if (down) {
            int y = (int)GRAPH_Y_TOP - 4 - r;
            disp_line_arg_t l = {
                .x0 = (uint16_t)(x + 5 - r), .y0 = (uint16_t)y,
                .x1 = (uint16_t)(x + 5 + r), .y1 = (uint16_t)y,
                .color = COLOR_RED, ._pad = 0
            };
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_LINE, &l);
        }
    }
}

/* Cache indices the BT list shows (all, or beacons and trackers only).
 * Returns how many were written to map[] (BT_MAX_SCAN_RESULTS long). */
static int bt_view(const ui_state_t *st, int *map)
{
    int n = 0;
    for (int i = 0; i < s_bt_count; i++)
        if (!st->bt_filter || bt_tag(&s_bt_cache[i]))
            map[n++] = i;
    return n;
}

static void render_bt(ui_state_t *st)
{
    draw_title(st->bt_filter ? "Beacons & Tags" : "Bluetooth Devices");
    clear_content();

    int map[BT_MAX_SCAN_RESULTS];
    int n = bt_view(st, map);
    if (n == 0) {
        draw_message(s_scanning_bt ? "Scanning..." :
                     st->bt_filter ? "None found" : "No devices found");
        return;
    }

    for (int i = 0; i < n; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const bt_scan_result_t *dev = &s_bt_cache[map[i]];
        /* Beacons: the kind first; a beacon rarely has a name. */
        const char *nm = st->bt_filter ? bt_tag(dev) : bt_label(dev);
        char line[20];
        snprintf(line, sizeof(line), "%-11.11s %4d", nm, (int)dev->rssi);
        draw_row(view, line, rssi_color(dev->rssi), i == st->sel);
    }
}

/* Warmer / colder: the newest three readings against the three before. */
static const char *bt_trend(void)
{
    int v[6], n = 0;
    for (int k = 1; k <= (int)DISP_WIDTH && n < 6; k++) {
        int x = (s_bt_graph_head - k + (int)DISP_WIDTH) % (int)DISP_WIDTH;
        if (s_bt_rssi_history[x] != 0) v[n++] = s_bt_rssi_history[x];
    }
    if (n < 6) return "";
    int d = (v[0] + v[1] + v[2]) - (v[3] + v[4] + v[5]);
    if (d >= 9)  return "Warmer";
    if (d <= -9) return "Colder";
    return "Steady";
}

#ifdef PICOOS_LED_ENABLE
static uint32_t s_finder_rgb = 0u;     /* LED color the finder wants; main */
#endif

/* Finder: one device, big RSSI, a meter and a trend, to walk towards it.
 * The LED follows the signal (green near, red far, off when lost). */
static void render_bt_finder(ui_state_t *st)
{
    int idx = st->bt_detail_idx;
    if (idx < 0 || idx >= s_bt_count) {
        draw_title("Finder");
        clear_content();
        draw_message("No data");
        return;
    }
    const bt_scan_result_t *dev = &s_bt_cache[idx];
    bool seen = s_bt_seen[idx];

    char tmp[20];
    snprintf(tmp, sizeof(tmp), "%s%.17s", watch_has_bt(dev->addr) ? "*" : "",
             bt_label(dev));
    draw_title(tmp);
    clear_content();

    uint8_t col = seen ? rssi_color(dev->rssi) : COLOR_GREY;
    if (seen) snprintf(tmp, sizeof(tmp), "%d", (int)dev->rssi);
    else      snprintf(tmp, sizeof(tmp), "Lost");
    int tw = (int)strlen(tmp) * 32;
    disp_text_arg_t big = {
        .x = (uint16_t)(((int)DISP_WIDTH - tw) / 2), .y = TITLE_H + 4,
        .color = col, .bg = COLOR_PANEL, .scale = 4, ._pad = 0, .str = tmp
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &big);

    /* Meter: -100 dBm empty, -30 dBm full. */
    int mx = 4, my = (int)TITLE_H + 40, mw = (int)DISP_WIDTH - 8, mh = 10;
    disp_rect_arg_t frame = {
        .x = (uint16_t)mx, .y = (uint16_t)my, .w = (uint16_t)mw, .h = (uint16_t)mh,
        .color = COLOR_GREY, .filled = 0, ._pad1 = 0, ._pad2 = 0
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &frame);
    if (seen) {
        int fill = ((int)dev->rssi + 100) * (mw - 2) / 70;
        if (fill < 1)      fill = 1;
        if (fill > mw - 2) fill = mw - 2;
        disp_rect_arg_t bar = {
            .x = (uint16_t)(mx + 1), .y = (uint16_t)(my + 1),
            .w = (uint16_t)fill, .h = (uint16_t)(mh - 2),
            .color = col, .filled = 1, ._pad1 = 0, ._pad2 = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bar);
    }

    int dm = seen ? bt_dist(dev) : -1;
    if (dm >= 0) snprintf(tmp, sizeof(tmp), "%-6s ~%d.%dm", bt_trend(), dm / 10, dm % 10);
    else         snprintf(tmp, sizeof(tmp), "%s", seen ? bt_trend() : "");
    disp_text_arg_t tr = {
        .x = SEL_X, .y = TITLE_H + 54,
        .color = COLOR_GREY, .bg = COLOR_PANEL, .scale = 2, ._pad = 0, .str = tmp
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &tr);

    /* The history graph only fits under the text on the larger panel. */
    if ((int)TITLE_H + 74 <= (int)GRAPH_Y_TOP)
        render_rssi_graph(s_bt_rssi_history, s_bt_graph_head);

#ifdef PICOOS_LED_ENABLE
    s_finder_rgb = !seen               ? 0x000000u :
                   dev->rssi > -67     ? 0x00FF00u :
                   dev->rssi > -70     ? 0xFFFF00u :
                   dev->rssi > -80     ? 0xFF6000u : 0xFF0000u;
#endif
}

/* Pairs list: two rows per pair, score and first device, then the second. */
#define PAIR_ROWS  ((int)VISIBLE_ROWS / 2)

static void adjust_pair_scroll(ui_state_t *st)
{
    if (st->sel < st->scroll) st->scroll = st->sel;
    if (st->sel >= st->scroll + PAIR_ROWS) st->scroll = st->sel - PAIR_ROWS + 1;
    if (st->scroll > s_pair_count - PAIR_ROWS) st->scroll = s_pair_count - PAIR_ROWS;
    if (st->scroll < 0) st->scroll = 0;
}

static uint8_t score_color(int score)
{
    return score >= 70 ? COLOR_GREEN : score >= 55 ? COLOR_YELLOW : COLOR_GREY;
}

static void render_bt_pairs(ui_state_t *st)
{
    draw_title("Pairs");
    clear_content();

    char line[24], la[20], lb[20];
    if (s_corr_n < CORR_MIN_WINDOWS) {
        snprintf(line, sizeof(line), "Learning %d/%ds", s_corr_n, CORR_MIN_WINDOWS);
        draw_message(line);
        draw_row(1, "Keep NetMon still", COLOR_GREY, false);
        return;
    }
    if (s_pair_count == 0) {
        draw_message("No pairs yet");
        draw_row(1, "Movement helps:", COLOR_GREY, false);
        draw_row(2, "devices that come", COLOR_GREY, false);
        draw_row(3, "& go together", COLOR_GREY, false);
        return;
    }

    for (int i = 0; i < s_pair_count; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= PAIR_ROWS) continue;
        const corr_pair_t *p = &s_pairs[i];
        bt_pair_label(&s_bt_cache[p->a], la, sizeof(la));
        bt_pair_label(&s_bt_cache[p->b], lb, sizeof(lb));
        /* '*' marks a phone/computer with an accessory. */
        snprintf(line, sizeof(line), "%3d%c%.14s", p->score, p->fit ? '*' : ' ', la);
        draw_row(view * 2, line, score_color(p->score), i == st->sel);
        snprintf(line, sizeof(line), "   +%.14s", lb);
        draw_row(view * 2 + 1, line, COLOR_GREY, false);
    }
}


/* One device's RSSI over the recorded windows, oldest at the left. */
static void draw_corr_trace(int slot, uint8_t color)
{
    int cw = (int)DISP_WIDTH / CORR_WINDOWS;
    if (cw < 1) cw = 1;
    int x0 = (int)DISP_WIDTH - cw * CORR_WINDOWS;
    if (x0 < 0) x0 = 0;
    for (int k = 0; k < s_corr_n; k++) {
        int pos = (int)((s_corr_win - (uint32_t)(s_corr_n - 1 - k)) % CORR_WINDOWS);
        int v = s_corr[slot][pos];
        int col = CORR_WINDOWS - s_corr_n + k;
        int x = x0 + col * cw;
        if (x + cw > (int)DISP_WIDTH) break;
        int y, h = 2;
        if (v < 0) {
            int str = (v + 100) * ((int)GRAPH_H - 2) / 70;
            if (str < 0)                  str = 0;
            if (str > (int)GRAPH_H - 2)   str = (int)GRAPH_H - 2;
            y = (int)GRAPH_Y_BOT - 1 - str;
        } else if (v == CORR_HELD) {
            y = (int)GRAPH_Y_BOT; h = 1;  /* present, not heard: baseline */
        } else {
            continue;                     /* absent */
        }
        disp_rect_arg_t r = {
            .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)cw, .h = (uint16_t)h,
            .color = color, .filled = 1, ._pad1 = 0, ._pad2 = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &r);
    }
}

static void fmt_corr(char *buf, size_t len, int v)
{
    if (v == CORR_NONE) snprintf(buf, len, "--");
    else                fmt_fixed(buf, len, v, 100);
}

/* One pair: both devices, the evidence, and both RSSI traces overlaid. */
static void render_bt_pair(ui_state_t *st)
{
    int a = bt_find(st->pair_addr[0]), b = bt_find(st->pair_addr[1]);
    corr_pair_t p;
    if (a < 0 || b < 0 || !corr_pair(a, b, &p)) {
        draw_title("Pair");
        clear_content();
        draw_message("No data");
        return;
    }
    char line[24], lab[20], v[12];
    snprintf(line, sizeof(line), "Pair %d%s", p.score, p.fit ? " *" : "");
    draw_title(line);
    clear_content();

    const bt_scan_result_t *da = &s_bt_cache[a], *db = &s_bt_cache[b];
    int row = 0;
#define PROW(color, ...)                                                    \
    do {                                                                    \
        if (row < (int)DETAIL_ROWS) {                                       \
            snprintf(line, sizeof(line), __VA_ARGS__);                      \
            draw_detail_row(row, line, (color));                            \
        }                                                                   \
        row++;                                                              \
    } while (0)
    bt_pair_label(da, lab, sizeof(lab));
    PROW(COLOR_YELLOW, "A %.17s", lab);
    PROW(COLOR_GREY,   "  %-9s %4d", bt_role_str(bt_role(da)),
         s_bt_seen[a] ? (int)da->rssi : 0);
    bt_pair_label(db, lab, sizeof(lab));
    PROW(COLOR_CYAN,   "B %.17s", lab);
    PROW(COLOR_GREY,   "  %-9s %4d", bt_role_str(bt_role(db)),
         s_bt_seen[b] ? (int)db->rssi : 0);
    PROW(COLOR_GREY,   "Together  %3d%%", p.together);
    fmt_corr(v, sizeof v, p.pres);
    PROW(score_color(p.pres), "Presence  %s", v);
    fmt_corr(v, sizeof v, p.rssi);
    PROW(score_color(p.rssi), "Signal    %s (%d)", v, p.samples);
    PROW(COLOR_GREY,   "Over %ds", s_corr_n);
#undef PROW

    disp_rect_arg_t bg = {
        .x = 0, .y = GRAPH_Y_TOP, .w = DISP_WIDTH, .h = GRAPH_H,
        .color = COLOR_PANEL, .filled = 1, ._pad1 = 0, ._pad2 = 0
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bg);
    draw_corr_trace(a, COLOR_YELLOW);
    draw_corr_trace(b, COLOR_CYAN);
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
    snprintf(title, sizeof(title), "%.18s", bt_label(dev));
    draw_title(title);
    clear_content();

    /* Build every row that has data, then draw the part scrolled into view.
     * Static, not on the stack: the app thread has only DEFAULT_STACK_SIZE
     * and snprintf needs much of it.  Only the UI thread renders. */
    static char    lines[BT_DETAIL_MAX_LINES][20];
    static uint8_t colors[BT_DETAIL_MAX_LINES];
    int     n = 0;
#define LINE(color, ...)                                                    \
    do {                                                                    \
        if (n < (int)BT_DETAIL_MAX_LINES) {                                 \
            snprintf(lines[n], sizeof(lines[n]), __VA_ARGS__);              \
            colors[n++] = (color);                                          \
        }                                                                   \
    } while (0)

    bool ble = dev->type == BT_DEVTYPE_BLE;
    const uint8_t *a = dev->addr;   /* LSB first: a[5] is printed first */

    LINE(rssi_color(dev->rssi), "RSSI: %ddBm", (int)dev->rssi);
    char range[20];
    if (rssi_range(s_bt_rssi_history, range, sizeof range))
        LINE(COLOR_GREY, "%s", range);

    bool ibeacon = dev->company_id == 0x004C && dev->mfr_data_len >= 23 &&
                   dev->mfr_data[0] == 0x02 && dev->mfr_data[1] == 0x15;
    int dm = s_bt_seen[idx] ? bt_dist(dev) : -1;
    if (dm >= 0)
        LINE(COLOR_GREY, "Dist: ~%d.%dm", dm / 10, dm % 10);

    if (ble) {
        const char *at = "BLE";
        if (dev->addr_type == BT_ADDR_PUBLIC) {
            at = "BLE Public";
        } else if (dev->addr_type == BT_ADDR_RANDOM) {
            switch (a[5] >> 6) {
            case 3:  at = "BLE Static";  break;
            case 1:  at = "BLE Private"; break;   /* resolvable: rotates */
            default: at = "BLE NonResv"; break;
            }
        }
        LINE(COLOR_GREY, "Type: %s", at);
    } else {
        LINE(COLOR_GREY, "Type: Classic");
    }
    const char *vnd = bt_vendor(dev);
    if (vnd) LINE(COLOR_GREY, "OUI:  %s", vnd);
    const char *tag = bt_tag(dev);
    if (tag && dev->company_id != 0x004C) LINE(COLOR_GREY, "Tag:  %s", tag);

    if (ble && dev->adv_type != BT_ADV_TYPE_NONE) {
        static const char *const adv[] = {
            "Conn+Scan", "Directed", "Scannable", "Beacon" };
        if (dev->adv_type < 4u)
            LINE(COLOR_GREY, "Adv: %s", adv[dev->adv_type]);
    }
    if (ble)
        LINE(COLOR_GREY, "Rate: %u pkt/s", (unsigned)dev->pkt_count);

    if (dev->tx_power != BT_TX_POWER_UNKNOWN)
        LINE(COLOR_GREY, "TxPwr: %ddBm", (int)dev->tx_power);

    if (dev->flags != BT_FLAGS_NONE) {
        const char *disc = (dev->flags & 0x01u) ? "Ltd" :
                           (dev->flags & 0x02u) ? "Gen" : "No";
        LINE(COLOR_GREY, "Disc:%s %s", disc,
             (dev->flags & 0x04u) ? "LE only" : "Dual");
    }

    if (dev->appearance != BT_APPEARANCE_NONE) {
        const char *ap = bt_appearance_str(dev->appearance);
        if (ap) LINE(COLOR_GREY, "Is: %s", ap);
        else    LINE(COLOR_GREY, "Is: 0x%04X", dev->appearance);
    }

    if (!ble) {
        LINE(COLOR_GREY, "Cls:  %s", bt_devclass_str(dev->dev_class));
        const char *mn = bt_cod_minor_str(dev->class_of_device);
        if (mn) LINE(COLOR_GREY, "Sub:  %s", mn);
        /* Service classes, wrapped onto as many rows as they need. */
        char svc[64];
        bt_cod_services(dev->class_of_device, svc, sizeof svc);
        const char *p = svc;
        const char *label = "Srv: ";
        while (*p) {
            int room = 19 - 5;
            int take = (int)strlen(p);
            if (take > room) {
                take = room;
                while (take > 0 && p[take] != ' ') take--;
                if (take == 0) take = room;
            }
            LINE(COLOR_GREY, "%s%.*s", label, take, p);
            label = "     ";
            p += take;
            while (*p == ' ') p++;
        }
    }

    if (dev->did_source != BT_DID_NONE) {
        const char *co = dev->did_source == BT_DID_SRC_BT_SIG
                             ? bt_company_name(dev->did_vendor) : NULL;
        if (co) LINE(COLOR_GREY, "Mfr:  %s", co);
        else    LINE(COLOR_GREY, "Mfr:  %s %04X",
                     dev->did_source == BT_DID_SRC_USB_IF ? "USB" : "BT",
                     dev->did_vendor);
        LINE(COLOR_GREY, "Prod: %04X v%04X", dev->did_product, dev->did_version);
    }

    if (dev->company_id != BT_COMPANY_NONE) {
        const char *co = bt_company_name(dev->company_id);
        if (co) LINE(COLOR_GREY, "Co:   %s", co);
        else    LINE(COLOR_GREY, "Co:   0x%04X", dev->company_id);

        const uint8_t *m = dev->mfr_data;
        if (dev->company_id == 0x004C && dev->mfr_data_len >= 1) {
            const char *ty = bt_apple_type_str(m[0]);
            if (ty) LINE(COLOR_GREY, "Msg:  %s", ty);
            else    LINE(COLOR_GREY, "Msg:  0x%02X", m[0]);
            if (ibeacon) {
                LINE(COLOR_GREY, "%02X%02X%02X%02X-%02X%02X",
                     m[2], m[3], m[4], m[5], m[6], m[7]);
                LINE(COLOR_GREY, "Maj:%u Min:%u",
                     (unsigned)((m[18] << 8) | m[19]),
                     (unsigned)((m[20] << 8) | m[21]));
            }
        } else if (dev->company_id == 0x0006 && dev->mfr_data_len >= 2 &&
                   m[0] == 0x01) {
            const char *ty = bt_ms_device_str(m[1] & 0x1Fu);
            if (ty) LINE(COLOR_GREY, "Dev:  %s", ty);
        }
    }

    if (dev->service_uuid != BT_SERVICE_NONE) {
        const char *sn = bt_service_name(dev->service_uuid);
        if (sn) LINE(COLOR_GREY, "Svc:  %s", sn);
        else    LINE(COLOR_GREY, "Svc:  0x%04X", dev->service_uuid);
    }
    if (dev->uuid32 != BT_UUID32_NONE)
        LINE(COLOR_GREY, "UUID: %08lX", (unsigned long)dev->uuid32);

    if (dev->svc_data_uuid != BT_SERVICE_NONE) {
        const char *sn = bt_service_name(dev->svc_data_uuid);
        if (sn) LINE(COLOR_GREY, "Data: %s", sn);
        else    LINE(COLOR_GREY, "Data: 0x%04X", dev->svc_data_uuid);
        char rd[20];
        if (bt_svc_data_reading(dev, rd, sizeof rd))
            LINE(COLOR_GREY, "%s", rd);
    }

    /* Seen: since first heard / since last heard (windows are ~1 s). */
    uint32_t first = s_bt_win - s_bt_first_win[idx];
    if (s_bt_last_win[idx] != 0u)
        LINE(COLOR_GREY, "Age:%lus Last:%lus",
             (unsigned long)(first * BT_WINDOW_MS / 1000u),
             (unsigned long)((s_bt_win - s_bt_last_win[idx]) * BT_WINDOW_MS / 1000u));

    LINE(COLOR_GREY, "%02X:%02X:%02X:%02X:%02X:%02X",
         a[5], a[4], a[3], a[2], a[1], a[0]);
#undef LINE

    /* Clamp the scroll here: only the renderer knows the row count. */
    int max_scroll = n - (int)DETAIL_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (st->detail_scroll > max_scroll) st->detail_scroll = max_scroll;
    if (st->detail_scroll < 0)          st->detail_scroll = 0;

    for (int r = 0; r < (int)DETAIL_ROWS && st->detail_scroll + r < n; r++)
        draw_detail_row(r, lines[st->detail_scroll + r],
                        colors[st->detail_scroll + r]);
    draw_scroll_marks(st->detail_scroll > 0, st->detail_scroll < max_scroll);

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
    snprintf(title, sizeof(title), "%s%.17s", watch_has_wifi(net) ? "*" : "",
             wifi_name(net));
    draw_title(title);
    clear_content();

    /* Build every row that has data, then draw the part scrolled into
     * view: with all optional rows present there are more than fit above
     * the RSSI graph (DETAIL_ROWS).  Static for the same reason as in
     * render_bt_detail(): the app stack is small, only the UI thread
     * renders. */
    static char    lines[WIFI_DETAIL_MAX_LINES][20];
    static uint8_t colors[WIFI_DETAIL_MAX_LINES];
    int  n = 0;
    char tmp[24];
#define WROW(color)                                                         \
    do {                                                                    \
        if (n < (int)WIFI_DETAIL_MAX_LINES) {                               \
            snprintf(lines[n], sizeof(lines[n]), "%s", tmp);                \
            colors[n++] = (color);                                          \
        }                                                                   \
    } while (0)

    snprintf(tmp, sizeof(tmp), "RSSI: %ddBm", (int)net->rssi);
    WROW(rssi_color(net->rssi));

    if (rssi_range(s_rssi_history, tmp, sizeof(tmp)))
        WROW(COLOR_GREY);

    if (net->noise != 0 || net->snr != 0) {
        snprintf(tmp, sizeof(tmp), "Noise:%d SNR:%ddB",
                 (int)net->noise, (int)net->snr);
        WROW(COLOR_GREY);
    }

    /* Channel: frequency when there is one, else the list of channels the
     * SSID was heard on. */
    if (s_wifi_chan_count[idx] == 1) {
        snprintf(tmp, sizeof(tmp), "Chan: %d (%dMHz)",
                 (int)net->channel, chan_mhz(net->channel));
    } else {
        int pos = snprintf(tmp, sizeof(tmp), "Chan: ");
        for (int i = 0; i < s_wifi_chan_count[idx]; i++) {
            int rem = 20 - pos;
            if (rem <= 1) break;
            pos += snprintf(tmp + pos, (size_t)rem,
                            i == 0 ? "%d" : ",%d", (int)s_wifi_chans[idx][i]);
        }
    }
    WROW(COLOR_GREY);

    if (net->max_rate != 0) {
        const char *std = (net->phy & WIFI_PHY_HT)   ? "11n" :
                          (net->phy & WIFI_PHY_OFDM) ? "11g" : "11b";
        snprintf(tmp, sizeof(tmp), "Mode: %s %s", std,
                 (net->phy & WIFI_PHY_HT40) ? "40MHz" : "20MHz");
        WROW(COLOR_GREY);
    }

    if (net->beacon_tu != 0) {
        /* 1 TU = 1.024 ms; rates are in 500 kb/s units. */
        snprintf(tmp, sizeof(tmp), "Bcn:%dms Max:%dM",
                 (int)net->beacon_tu * 1024 / 1000, (int)net->max_rate / 2);
        WROW(COLOR_GREY);
    }

    snprintf(tmp, sizeof(tmp), "Sec: %s", auth_label(net->auth_mode));
    WROW(auth_color(net->auth_mode));

    const char *vnd = mac_vendor(net->bssid[0], net->bssid[1], net->bssid[2]);
    if (vnd) {
        snprintf(tmp, sizeof(tmp), "Vnd: %s", vnd);
        WROW(COLOR_GREY);
    }

    /* Channel congestion: other networks heard this scan on the same
     * channel, or within 4 channels (overlapping 20 MHz at 2.4 GHz). */
    int coch = 0, adj = 0;
    for (int i = 0; i < s_wifi_ucount; i++) {
        if (i == idx || !s_wifi_seen[i]) continue;
        int d = abs((int)s_wifi_cache[i].channel - (int)net->channel);
        if (d == 0)     coch++;
        else if (d <= 4) adj++;
    }
    snprintf(tmp, sizeof(tmp), "APs:%d CoCh:%d Adj:%d",
             s_wifi_aps[idx], coch, adj);
    WROW(COLOR_GREY);

    /* BSSID of the strongest AP for this SSID */
    const uint8_t *b = net->bssid;
    snprintf(tmp, sizeof(tmp), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    WROW(COLOR_GREY);
#undef WROW

    /* B scrolls a row at a time and wraps back to the top. */
    int max_scroll = n > (int)DETAIL_ROWS ? n - (int)DETAIL_ROWS : 0;
    if (st->detail_scroll > max_scroll) st->detail_scroll = 0;
    if (st->detail_scroll < 0)          st->detail_scroll = 0;
    for (int r = 0; r < (int)DETAIL_ROWS && st->detail_scroll + r < n; r++)
        draw_detail_row(r, lines[st->detail_scroll + r],
                        colors[st->detail_scroll + r]);
    draw_scroll_marks(st->detail_scroll > 0, st->detail_scroll < max_scroll);

    render_rssi_graph(s_rssi_history, s_graph_head);
}

/* Every BSSID of the detail panel's network: channel, RSSI and the last
 * three octets (the first three are usually the same vendor OUI). */
static void render_wifi_aps(ui_state_t *st)
{
    char title[20];
    snprintf(title, sizeof(title), "APs %.14s", wifi_name(&s_detail_key));
    draw_title(title);
    clear_content();

    if (s_ap_count == 0) {
        draw_message("Scanning...");
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const ap_entry_t *ap = &s_ap_list[i];
        char line[20];
        snprintf(line, sizeof(line), "%2d %4d %02X:%02X:%02X", ap->channel,
                 (int)ap->rssi, ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        draw_row(view, line, rssi_color(ap->rssi), i == st->sel);
    }
}

/* Watched entries (status, kind, name), then the latest events. */
static void render_watch(ui_state_t *st)
{
    draw_title("Watch");
    clear_content();

    if (s_watch_count == 0) {
        draw_message("List empty");
        draw_row(1, "Add: A on WiFi", COLOR_GREY, false);
        draw_row(2, "detail, X in BT", COLOR_GREY, false);
        draw_row(3, "finder", COLOR_GREY, false);
        return;
    }

    char line[24];
    int  row = 0;
    for (int i = 0; i < s_watch_count; i++) {
        int view = i - st->scroll;
        if (view < 0 || view >= (int)VISIBLE_ROWS) continue;
        const watch_t *w = &s_watch[i];
        static const char st_ch[] = { '?', '+', '-' };
        static const uint8_t st_col[] = { COLOR_GREY, COLOR_GREEN, COLOR_RED };
        snprintf(line, sizeof(line), "%c%c %.14s", st_ch[w->state],
                 w->kind == 'B' ? 'B' : 'W', w->name);
        draw_row(view, line, st_col[w->state], i == st->sel);
        row = view + 1;
    }

    /* Events, newest first, in the rows left over. */
    uint32_t now = now_ms();
    for (int k = 0; k < s_watch_ev_count && row < (int)VISIBLE_ROWS; k++, row++) {
        const watch_event_t *e =
            &s_watch_ev[(s_watch_ev_head - 1 - k + 2 * WATCH_EVENTS) % WATCH_EVENTS];
        uint32_t age = (now - e->ms) / 1000u;
        if (age < 600u)
            snprintf(line, sizeof(line), "%c%-10.10s%4lus", e->arrived ? '+' : '-',
                     e->name, (unsigned long)age);
        else
            snprintf(line, sizeof(line), "%c%-10.10s%4lum", e->arrived ? '+' : '-',
                     e->name, (unsigned long)(age / 60u));
        draw_row(row, line, COLOR_GREY, false);
    }
}

static void render_about(void)
{
    draw_title("About");
    clear_content();

    uint32_t used = 0u, free_bytes = 0u, largest = 0u;
    kmem_stats(&used, &free_bytes, &largest);

    char lines[7][20];
    snprintf(lines[0], sizeof(lines[0]), "NetMon: v" NETMON_VERSION);
    snprintf(lines[1], sizeof(lines[1]), "picoOS: v%d.%d.%d",
             PICOOS_VERSION_MAJOR, PICOOS_VERSION_MINOR, PICOOS_VERSION_EDIT);
    snprintf(lines[2], sizeof(lines[2]), "Memory:");
    snprintf(lines[3], sizeof(lines[3]), " Free: %5u B", (unsigned)free_bytes);
    snprintf(lines[4], sizeof(lines[4]), " Used: %5u B", (unsigned)used);
    snprintf(lines[5], sizeof(lines[5]), " Max:  %5u B", (unsigned)largest);

    kmutex_lock(&s_log_lock);
    snprintf(lines[6], sizeof(lines[6]), "Log: W%d B%d", s_wlog_n, s_blog_n);
    kmutex_unlock(&s_log_lock);

    for (int i = 0; i < 7; i++) {
        int y = (int)TITLE_H + i * (int)ROW_H + ((int)ROW_H - 16) / 2;
        disp_text_arg_t t = {
            .x = TEXT_X, .y = (uint16_t)y,
            .color = COLOR_GREY, .bg = COLOR_PANEL, .scale = 2, ._pad = 0,
            .str = lines[i]
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
    }
}

#define CHAN_SLOT_W   ((int)DISP_WIDTH / SURVEY_CHANS)
#define CHAN_X0       (((int)DISP_WIDTH - CHAN_SLOT_W * SURVEY_CHANS) / 2)
#define CHAN_TEXT_Y   ((int)DISP_HEIGHT - 3 * (int)DETAIL_ROW_H)
#define CHAN_LABEL_Y  (CHAN_TEXT_Y - 10)
#define CHAN_BAR_BOT  (CHAN_LABEL_Y - 2)
#define CHAN_BAR_TOP  ((int)TITLE_H + 4)
#define CHAN_LOAD_MAX 60          /* load at full bar height */

static void draw_text_at(int x, int y, const char *s, uint8_t fg, uint8_t bg,
                         uint8_t scale)
{
    disp_text_arg_t t = {
        .x = (uint16_t)x, .y = (uint16_t)y,
        .color = fg, .bg = bg, .scale = scale, ._pad = 0,
        .str = s
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);
}

/* History of the selected channel: load bars (colored by load) with the
 * noise floor as white dots (-100 dBm at the bottom, -60 at the top). */
static void render_chan_history(void)
{
    int top = CHAN_BAR_TOP, bot = CHAN_LABEL_Y + 8;
    int span = bot - top + 1;
    for (int x = 0; x < (int)DISP_WIDTH; x++) {
        int gap_rel = (x - s_chan_hist_head + (int)DISP_WIDTH) % (int)DISP_WIDTH;
        if (gap_rel < (int)GRAPH_GAP_W) continue;
        int lv = s_chan_hist_load[x];
        if (lv == 0) continue;
        int load = lv - 1;
        int h = load * span / CHAN_LOAD_MAX;
        if (h < 1)    h = 1;
        if (h > span) h = span;
        disp_rect_arg_t bar = {
            .x = (uint16_t)x, .y = (uint16_t)(bot - h + 1), .w = 1, .h = (uint16_t)h,
            .color = load_color(load), .filled = 1, ._pad1 = 0, ._pad2 = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bar);

        int nv = s_chan_hist_noise[x];
        if (nv != 0) {
            int ny = bot - (nv + 100) * (span - 2) / 40 - 1;
            if (ny < top)     ny = top;
            if (ny > bot - 1) ny = bot - 1;
            disp_rect_arg_t dot = {
                .x = (uint16_t)x, .y = (uint16_t)ny, .w = 1, .h = 2,
                .color = COLOR_WHITE, .filled = 1, ._pad1 = 0, ._pad2 = 0
            };
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &dot);
        }
    }
}

static void render_channels(ui_state_t *st)
{
    if (st->chan_hist) {
        char title[20];
        snprintf(title, sizeof(title), "Ch%d History", st->sel + 1);
        draw_title(title);
    } else {
        draw_title("Channels");
    }
    clear_content();

    chan_stat_t cs[SURVEY_CHANS];
    int         load[SURVEY_CHANS];
    bool        any = false;
    for (int ch = 0; ch < SURVEY_CHANS; ch++) {
        cs[ch] = survey_chan(ch);
        if (cs[ch].aps > 0) any = true;
    }
    for (int ch = 0; ch < SURVEY_CHANS; ch++)
        load[ch] = survey_load(cs, ch);

    if (!any) {
        draw_message(s_scanning_wifi ? "Scanning..." : "No networks found");
        return;
    }

    int bar_span = CHAN_BAR_BOT - CHAN_BAR_TOP + 1;
    if (st->chan_hist) render_chan_history();
    for (int ch = 0; ch < SURVEY_CHANS && !st->chan_hist; ch++) {
        int x = CHAN_X0 + ch * CHAN_SLOT_W;
        int h = load[ch] * bar_span / CHAN_LOAD_MAX;
        if (h < 1)        h = 1;
        if (h > bar_span) h = bar_span;
        disp_rect_arg_t bar = {
            .x = (uint16_t)(x + 2), .y = (uint16_t)(CHAN_BAR_BOT - h + 1),
            .w = (uint16_t)(CHAN_SLOT_W - 4), .h = (uint16_t)h,
            .color = load_color(load[ch]), .filled = 1, ._pad1 = 0, ._pad2 = 0
        };
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &bar);

        char num[4];
        snprintf(num, sizeof(num), "%d", ch + 1);
        int lx = x + (CHAN_SLOT_W - (int)strlen(num) * 8) / 2;
        bool sel = ch == st->sel;
        draw_text_at(lx, CHAN_LABEL_Y, num, sel ? COLOR_WHITE : COLOR_GREY,
                     sel ? COLOR_RED : COLOR_PANEL, 1);
    }

    /* Least-loaded of the non-overlapping channels 1, 6 and 11. */
    int best = 0;
    if (load[5]  < load[best]) best = 5;
    if (load[10] < load[best]) best = 10;

    const chan_stat_t *c = &cs[st->sel];
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "Ch%d Load:%d", st->sel + 1, load[st->sel]);
    draw_text_at(SEL_X, CHAN_TEXT_Y + 1, tmp, load_color(load[st->sel]),
                 COLOR_PANEL, 2);

    if (c->aps > 0)
        snprintf(tmp, sizeof(tmp), "APs:%d Sig:%d", c->aps, c->best_rssi);
    else
        snprintf(tmp, sizeof(tmp), "APs:0");
    draw_text_at(SEL_X, CHAN_TEXT_Y + (int)DETAIL_ROW_H + 1, tmp, COLOR_GREY,
                 COLOR_PANEL, 2);

    if (c->noise != 0)
        snprintf(tmp, sizeof(tmp), "N:%d", c->noise);
    else
        snprintf(tmp, sizeof(tmp), "N:--");
    draw_text_at(SEL_X, CHAN_TEXT_Y + 2 * (int)DETAIL_ROW_H + 1, tmp,
                 noise_color(c->noise), COLOR_PANEL, 2);

    snprintf(tmp, sizeof(tmp), "Best:%d", best + 1);
    draw_text_at((int)DISP_WIDTH - (int)strlen(tmp) * 16 - (int)SEL_X,
                 CHAN_TEXT_Y + 2 * (int)DETAIL_ROW_H + 1, tmp, COLOR_GREY,
                 COLOR_PANEL, 2);
}

static void render_screen(ui_state_t *st)
{
    switch (st->screen) {
    case SCR_HOME: case SCR_WIFI_MENU: case SCR_BT_MENU:
                          render_menu(st);        break;
    case SCR_WIFI:        render_wifi(st);        break;
    case SCR_BT:          render_bt(st);          break;
    case SCR_BT_DETAIL:   render_bt_detail(st);   break;
    case SCR_ABOUT:       render_about();         break;
    case SCR_WIFI_DETAIL: render_wifi_detail(st); break;
    case SCR_CHANNELS:    render_channels(st);    break;
    case SCR_WIFI_APS:    render_wifi_aps(st);    break;
    case SCR_BT_FINDER:   render_bt_finder(st);   break;
    case SCR_WATCH:       render_watch(st);       break;
    case SCR_BT_PAIRS:    render_bt_pairs(st);    break;
    case SCR_BT_PAIR:     render_bt_pair(st);     break;
    }
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
}

/* ---- navigation ----------------------------------------------------------- */

typedef enum { RADIO_NONE = 0, RADIO_WIFI, RADIO_BT, RADIO_WATCH } radio_t;
static radio_t screen_radio(screen_t s);
static bool    s_log_flush = false;   /* main: save the logs now */

/* Lock guarding the scan cache a screen reads, or NULL if it reads none. */
static kmutex_t *screen_lock(screen_t s)
{
    switch (s) {
    case SCR_WIFI: case SCR_WIFI_DETAIL: case SCR_CHANNELS: case SCR_WIFI_APS:
        return &s_wifi_lock;
    case SCR_BT:   case SCR_BT_DETAIL:   case SCR_BT_FINDER:
    case SCR_BT_PAIRS: case SCR_BT_PAIR:
        return &s_bt_lock;
    case SCR_WATCH:
        return &s_watch_lock;
    default:                             return NULL;
    }
}

/* Open screen `s` from a menu (no lock is held there), saving the menu's
 * cursor for the way back. */
static void enter_screen(ui_state_t *st, screen_t s)
{
    int m = screen_menu(st->screen);
    if (m >= 0) st->menu_sel[m] = st->sel;
    st->screen       = s;
    st->sel          = 0;
    st->scroll       = 0;
    st->dirty        = true;

    if (s == SCR_WIFI) {
        kmutex_lock(&s_wifi_lock);
        s_wifi_ucount = 0;
        memset(s_wifi_chan_count, 0, sizeof(s_wifi_chan_count));
        memset(s_wifi_seen, 0, sizeof(s_wifi_seen));
        memset(s_wifi_last_win, 0, sizeof(s_wifi_last_win));
        s_ap_key_set = false;
        kmutex_unlock(&s_wifi_lock);
    } else if (s == SCR_CHANNELS) {
        kmutex_lock(&s_wifi_lock);
        memset(s_survey, 0, sizeof(s_survey));
        s_survey_head = 0;
        kmutex_unlock(&s_wifi_lock);
        st->chan_hist = false;
        chan_hist_reset();
    } else if (s == SCR_BT || s == SCR_BT_PAIRS) {
        kmutex_lock(&s_bt_lock);
        s_bt_count    = 0;
        s_bt_scan_end = false;
        memset(s_bt_seen, 0, sizeof(s_bt_seen));
        memset(s_bt_last_win, 0, sizeof(s_bt_last_win));
        corr_reset();
        kmutex_unlock(&s_bt_lock);
        s_pair_count = 0;
    }
    s_active_screen = s;
}

/* Y: go up to the screen's menu, stopping its radio, and put the menu's
 * cursor back where it was. */
static void leave_screen(ui_state_t *st)
{
    screen_t from = st->screen;
    screen_t to   = screen_parent(from);

    /* Publish the menu before stopping the radio: the stop wakes the
     * worker out of *_scan_wait() at once, and it must then see a screen
     * with no radio, not the old one, or it restarts the scan. */
    s_active_screen = to;
    radio_t r = screen_radio(from);
    if (r == RADIO_WIFI || r == RADIO_WATCH) wifi_scan_stop();
    if (r == RADIO_BT   || r == RADIO_WATCH) bt_scan_stop();
    if (r != RADIO_NONE) s_log_flush = true;

    int m = screen_menu(to);
    st->screen = to;
    st->sel    = st->menu_sel[m];
    st->scroll = 0;
    adjust_scroll(st, s_menus[m].count);
    st->dirty  = true;
}

static void handle_input(ui_state_t *st, uint8_t pressed)
{
    if (!pressed) return;

    switch (st->screen) {
    case SCR_HOME: case SCR_WIFI_MENU: case SCR_BT_MENU: {
        const menu_t *m = &s_menus[screen_menu(st->screen)];
        if ((pressed & DISP_BTN_A) && st->sel > 0) {
            st->sel--; st->dirty = true;
        }
        if ((pressed & DISP_BTN_B) && st->sel < m->count - 1) {
            st->sel++; st->dirty = true;
        }
        adjust_scroll(st, m->count);
        if (pressed & DISP_BTN_X) {
            const menu_item_t *it = &m->items[st->sel];
            st->bt_filter = it->bt_filter;
            enter_screen(st, it->target);
        } else if ((pressed & DISP_BTN_Y) && st->screen != SCR_HOME) {
            leave_screen(st);
        }
        break;
    }
    case SCR_WIFI: {
        if (s_wifi_ucount > 0) {
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
                st->detail_scroll = 0;
                st->screen      = SCR_WIFI_DETAIL;
                st->dirty       = true;
                memset(s_rssi_history, 0, sizeof(s_rssi_history));
                s_graph_head = 0;
                /* Main holds s_wifi_lock here (screen_lock(SCR_WIFI)). */
                s_detail_key = s_wifi_cache[st->sel];
                s_ap_key     = s_detail_key;
                s_ap_key_set = true;
                s_ap_count   = 0;
            }
        }
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    }
    case SCR_WIFI_DETAIL:
        if ((pressed & DISP_BTN_A) && st->detail_idx >= 0 &&
            st->detail_idx < s_wifi_ucount) {
            watch_toggle_wifi(&s_wifi_cache[st->detail_idx]);
            st->dirty = true;
        }
        /* render_wifi_detail() wraps the scroll back to the top. */
        if (pressed & DISP_BTN_B) { st->detail_scroll++; st->dirty = true; }
        if (pressed & DISP_BTN_X) {
            st->screen = SCR_WIFI_APS;
            st->sel    = 0;
            st->scroll = 0;
            st->dirty  = true;
        }
        if (pressed & DISP_BTN_Y) {
            st->screen   = SCR_WIFI;
            st->sel      = st->wifi_sel;
            st->scroll   = st->wifi_scroll;
            st->dirty    = true;
            s_ap_key_set = false;
        }
        break;
    case SCR_WIFI_APS:
        if ((pressed & DISP_BTN_A) && st->sel > 0) {
            st->sel--;
            adjust_scroll(st, s_ap_count);
            st->dirty = true;
        }
        if ((pressed & DISP_BTN_B) && st->sel < s_ap_count - 1) {
            st->sel++;
            adjust_scroll(st, s_ap_count);
            st->dirty = true;
        }
        if (pressed & DISP_BTN_Y) {
            st->screen = SCR_WIFI_DETAIL;
            st->dirty  = true;
        }
        break;
    case SCR_BT: {
        int map[BT_MAX_SCAN_RESULTS];
        int n = bt_view(st, map);
        if (n > 0) {
            if (pressed & DISP_BTN_A) {
                if (st->sel > 0) {
                    st->sel--;
                    adjust_scroll(st, n);
                    st->dirty = true;
                }
            }
            if (pressed & DISP_BTN_B) {
                if (st->sel < n - 1) {
                    st->sel++;
                    adjust_scroll(st, n);
                    st->dirty = true;
                }
            }
            if ((pressed & DISP_BTN_X) && st->sel < n) {
                st->bt_sel        = st->sel;
                st->bt_scroll     = st->scroll;
                st->bt_detail_idx = map[st->sel];
                memcpy(st->bt_detail_addr, s_bt_cache[map[st->sel]].addr, BT_ADDR_LEN);
                st->detail_scroll = 0;
                st->screen        = SCR_BT_DETAIL;
                st->dirty         = true;
                memset(s_bt_rssi_history, 0, sizeof(s_bt_rssi_history));
                s_bt_graph_head   = 0;
            }
        }
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    }
    case SCR_BT_DETAIL:
        /* render_bt_detail() clamps the scroll to the rows it has. */
        if (pressed & DISP_BTN_A) { st->detail_scroll--; st->dirty = true; }
        if (pressed & DISP_BTN_B) { st->detail_scroll++; st->dirty = true; }
        if (pressed & DISP_BTN_X) { st->screen = SCR_BT_FINDER; st->dirty = true; }
        if (pressed & DISP_BTN_Y) {
            st->screen = SCR_BT;
            st->sel    = st->bt_sel;
            st->scroll = st->bt_scroll;
            st->dirty  = true;
        }
        break;
    case SCR_BT_FINDER:
        if ((pressed & DISP_BTN_X) && st->bt_detail_idx >= 0 &&
            st->bt_detail_idx < s_bt_count) {
            watch_toggle_bt(&s_bt_cache[st->bt_detail_idx]);
            st->dirty = true;
        }
        if (pressed & DISP_BTN_Y) { st->screen = SCR_BT_DETAIL; st->dirty = true; }
        break;
    case SCR_BT_PAIRS:
        if ((pressed & DISP_BTN_A) && st->sel > 0) {
            st->sel--; st->dirty = true;
        }
        if ((pressed & DISP_BTN_B) && st->sel < s_pair_count - 1) {
            st->sel++; st->dirty = true;
        }
        adjust_pair_scroll(st);
        if ((pressed & DISP_BTN_X) && st->sel < s_pair_count) {
            const corr_pair_t *p = &s_pairs[st->sel];
            memcpy(st->pair_addr[0], s_bt_cache[p->a].addr, BT_ADDR_LEN);
            memcpy(st->pair_addr[1], s_bt_cache[p->b].addr, BT_ADDR_LEN);
            st->pair_sel    = st->sel;
            st->pair_scroll = st->scroll;
            st->screen      = SCR_BT_PAIR;
            st->dirty       = true;
        }
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    case SCR_BT_PAIR:
        if (pressed & DISP_BTN_Y) {
            st->screen = SCR_BT_PAIRS;
            st->sel    = st->pair_sel;
            st->scroll = st->pair_scroll;
            st->dirty  = true;
        }
        break;
    case SCR_CHANNELS:
        if ((pressed & DISP_BTN_A) && st->sel > 0) {
            st->sel--; st->dirty = true;
            chan_hist_reset();
        }
        if ((pressed & DISP_BTN_B) && st->sel < SURVEY_CHANS - 1) {
            st->sel++; st->dirty = true;
            chan_hist_reset();
        }
        if (pressed & DISP_BTN_X) {
            st->chan_hist = !st->chan_hist;
            st->dirty     = true;
        }
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    case SCR_WATCH:
        /* Main holds s_watch_lock here (screen_lock(SCR_WATCH)). */
        if ((pressed & DISP_BTN_A) && st->sel > 0) {
            st->sel--;
            adjust_scroll(st, s_watch_count);
            st->dirty = true;
        }
        if ((pressed & DISP_BTN_B) && st->sel < s_watch_count - 1) {
            st->sel++;
            adjust_scroll(st, s_watch_count);
            st->dirty = true;
        }
        if ((pressed & DISP_BTN_X) && st->sel < s_watch_count) {
            watch_remove_locked(st->sel);
            if (st->sel >= s_watch_count && st->sel > 0) st->sel--;
            adjust_scroll(st, s_watch_count);
            st->dirty = true;
        }
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    case SCR_ABOUT:
        if (pressed & DISP_BTN_Y) leave_screen(st);
        break;
    }
}

/* ---- scan worker ---------------------------------------------------------- *
 * Runs picoOS continuous scanning for the radio the active screen shows and
 * merges each window into the caches, so the UI thread stays responsive.
 * The window lists come from *_scan_wait() and are ours until the next wait,
 * so they are read with no lock; only the merge into the caches takes
 * s_wifi_lock / s_bt_lock.  s_active_screen is the only lock-free field
 * (single writer, single aligned word).
 *
 * The scan is owned by this process: if netmon is killed, picoOS stops it.  */

/* RADIO_WATCH (the Watch screen) alternates WiFi and BT. */
#define WATCH_WIFI_WINDOWS  3u      /* per WiFi turn on the Watch screen */
#define WATCH_BT_WINDOWS   12u      /* per BT turn: one whole inquiry    */

static radio_t screen_radio(screen_t s)
{
    switch (s) {
    case SCR_WIFI: case SCR_WIFI_DETAIL: case SCR_CHANNELS: case SCR_WIFI_APS:
        return RADIO_WIFI;
    case SCR_BT:   case SCR_BT_DETAIL:   case SCR_BT_FINDER:
    case SCR_BT_PAIRS: case SCR_BT_PAIR:
        return RADIO_BT;
    case SCR_WATCH:
        return RADIO_WATCH;
    default:                             return RADIO_NONE;
    }
}

static void radio_stop(radio_t r)
{
    if (r == RADIO_WIFI) wifi_scan_stop();
    if (r == RADIO_BT)   bt_scan_stop();
}

/* Start continuous scanning.  False if picoOS refused (another scan is
 * running, or the BT controller is not up yet); the caller retries. */
static bool radio_start(radio_t r)
{
    if (r == RADIO_WIFI) {
        kmutex_lock(&s_wifi_lock);
        s_scanning_wifi = true;
        kmutex_unlock(&s_wifi_lock);
        return wifi_scan_start(NULL, NULL) == 0;
    }
    kmutex_lock(&s_bt_lock);
    s_scanning_bt = true;
    kmutex_unlock(&s_bt_lock);
    return bt_scan_start(NULL, NULL) == 0;
}

static void merge_wifi_window(const wifi_scan_list_t *l)
{
    int idx[WIFI_MAX_SCAN_RESULTS];
    int aps[WIFI_MAX_SCAN_RESULTS];
    int n = build_dedup(l->items, l->count, idx, aps, WIFI_MAX_SCAN_RESULTS);

    kmutex_lock(&s_wifi_lock);
    merge_into_cache(l->items, idx, aps, n, l->seq);
    ap_track(l->items, l->count, l->seq);
    survey_window(l->items, l->count);
    s_scanning_wifi = false;
    s_new_wifi_data = true;
    kmutex_unlock(&s_wifi_lock);

    watch_wifi_window(l->items, l->count);
    log_wifi_window(l->items, l->count);
}

static void merge_bt_window(const bt_scan_list_t *l)
{
    kmutex_lock(&s_bt_lock);
    merge_bt_into_cache(l->items, l->count, l->seq);
    s_scanning_bt = false;
    s_new_bt_data = true;
    s_bt_scan_end = true;
    kmutex_unlock(&s_bt_lock);

    watch_bt_window(l->items, l->count);
    log_bt_window(l->items, l->count);
}

static void scan_worker_entry(void *arg)
{
    (void)arg;
    radio_t running = RADIO_NONE;
    radio_t phase   = RADIO_WIFI;   /* Watch screen: radio taking its turn */
    unsigned turn   = 0u;           /* windows into that turn              */

    for (;;) {
        radio_t want = screen_radio(s_active_screen);
        if (want == RADIO_WATCH) {
            want = phase;
        } else {
            phase = RADIO_WIFI;
            turn  = 0u;
        }

        if (want != running) {
            radio_stop(running);
            running = RADIO_NONE;
            if (want == RADIO_NONE) {
                sys_sleep(100);
                continue;
            }
            if (!radio_start(want)) {
                sys_sleep(SCAN_RETRY_MS);
                continue;
            }
            running = want;
        }

        /* Blocks until the next window (at most ~1 s), or returns STOPPED
         * when leave_screen() stops the radio. */
        int rc;
        if (running == RADIO_WIFI) {
            const wifi_scan_list_t *l;
            rc = wifi_scan_wait(&l);
            if (rc == 0) merge_wifi_window(l);
        } else {
            const bt_scan_list_t *l;
            rc = bt_scan_wait(&l);
            if (rc == 0) merge_bt_window(l);
        }
        if (rc != 0) {
            running = RADIO_NONE;   /* stopped: re-evaluate */
        } else if (screen_radio(s_active_screen) == RADIO_WATCH &&
                   ++turn >= (phase == RADIO_WIFI ? WATCH_WIFI_WINDOWS
                                                  : WATCH_BT_WINDOWS)) {
            phase = phase == RADIO_WIFI ? RADIO_BT : RADIO_WIFI;
            turn  = 0u;
        }
    }
}

/* ---- entry point ---------------------------------------------------------- */

void netmon_entry(void *arg)
{
    (void)arg;

    kmutex_init(&s_wifi_lock);
    kmutex_init(&s_bt_lock);
    kmutex_init(&s_watch_lock);
    kmutex_init(&s_log_lock);
    s_active_screen = SCR_HOME;
    s_watch_count = 0;
    s_watch_ev_count = 0;
    s_wlog_n = 0;
    s_blog_n = 0;
    s_log_dirty = false;
    watch_load();

    dev_open(DEV_DISPLAY);
#ifdef PICOOS_LED_ENABLE
    dev_open(DEV_LED);
    dev_ioctl(DEV_LED, IOCTL_LED_OFF, NULL);
#endif

    uint8_t bg = COLOR_PANEL;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

    /* Spawn scan worker to receive WiFi/BT scan windows in the background,
     * keeping the UI thread free for input polling and rendering.
     * Runs AFFINITY_ANY: CYW43 and BTstack run in the async-context IRQ on
     * core 0, so radio calls must not be pinned to C1. */
    pcb_t *proc = task_find_process(sys_getpid());
    if (task_create_thread(proc, "scan-worker",
                           scan_worker_entry, NULL,
                           5u, DEFAULT_STACK_SIZE) == NULL) {
        /* Thread pool or kernel heap full: without the worker no scan
         * data ever arrives, so say why instead of showing empty lists. */
        printf("[netmon] cannot create scan-worker (pool or heap full)\r\n");
        draw_message("No scan thread");
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
        for (;;) sys_sleep(1000);
    }

    ui_state_t st = {
        .screen        = SCR_HOME,
        .sel           = 0,
        .scroll        = 0,
        .menu_sel      = {0},
        .wifi_sel      = 0,
        .wifi_scroll   = 0,
        .detail_idx    = 0,
        .bt_sel        = 0,
        .bt_scroll     = 0,
        .bt_detail_idx = 0,
        .detail_scroll = 0,
        .dirty         = true,
    };
    uint8_t  prev_btns   = 0u;
    uint32_t seen_seq    = s_watch_seq;
    uint32_t last_save   = now_ms();
    uint32_t last_tick   = 0u;       /* Watch screen: last 1 s refresh   */
    uint32_t save_retry  = 0u;       /* watch file: last failed save      */
    bool     save_wait   = false;
#ifdef PICOOS_LED_ENABLE
    uint32_t led_rgb     = 0u;       /* what the LED shows now            */
    uint32_t flash_rgb   = 0u;       /* watch event flash color           */
    uint32_t flash_until = 0u;
#endif

    for (;;) {
        /* Hold the active screen's cache lock across data intake, input
         * handling and rendering so the worker cannot merge mid-read.
         * Captured up front because handle_input() may change st.screen
         * (enter_screen() takes the target lock itself, from a menu only). */
        kmutex_t *lk = screen_lock(st.screen);
        if (lk) kmutex_lock(lk);

        if (s_new_wifi_data && st.screen == SCR_CHANNELS) {
            s_new_wifi_data = false;
            chan_hist_push(st.sel);
            st.dirty = true;
        }

        if (s_new_wifi_data &&
            (st.screen == SCR_WIFI || st.screen == SCR_WIFI_DETAIL ||
             st.screen == SCR_WIFI_APS)) {
            s_new_wifi_data = false;
            int ucount = s_wifi_ucount;
            /* The detail network's slot was reused for another network:
             * go back to the list rather than show the wrong one. */
            if (st.screen != SCR_WIFI &&
                (st.detail_idx < 0 || st.detail_idx >= ucount ||
                 !same_net(&s_wifi_cache[st.detail_idx], &s_detail_key))) {
                st.screen    = SCR_WIFI;
                st.sel       = st.wifi_sel;
                st.scroll    = st.wifi_scroll;
                s_ap_key_set = false;
            }
            if (st.screen == SCR_WIFI_APS) {
                if (st.sel >= s_ap_count)
                    st.sel = s_ap_count > 0 ? s_ap_count - 1 : 0;
                adjust_scroll(&st, s_ap_count);
            }
            if (st.screen == SCR_WIFI) {
                if (st.sel >= ucount)
                    st.sel = ucount > 0 ? ucount - 1 : 0;
                adjust_scroll(&st, ucount);
            }
            if ((st.screen == SCR_WIFI_DETAIL || st.screen == SCR_WIFI_APS) &&
                st.detail_idx >= 0 && st.detail_idx < ucount) {
                /* Not heard this window: record a gap (0) rather than
                 * repeating the stale cached reading. */
                s_rssi_history[s_graph_head] = s_wifi_seen[st.detail_idx]
                    ? (int8_t)s_wifi_cache[st.detail_idx].rssi : 0;
                s_graph_head = (s_graph_head + 1) % (int)DISP_WIDTH;
            }
            st.dirty = true;
        }

        if (s_new_bt_data &&
            (st.screen == SCR_BT || st.screen == SCR_BT_DETAIL ||
             st.screen == SCR_BT_FINDER)) {
            s_new_bt_data = false;
            int bcount = s_bt_count;
            /* The detail device's slot was reused for another device:
             * go back to the list rather than show the wrong one. */
            if (st.screen != SCR_BT &&
                (st.bt_detail_idx < 0 || st.bt_detail_idx >= bcount ||
                 memcmp(s_bt_cache[st.bt_detail_idx].addr, st.bt_detail_addr,
                        BT_ADDR_LEN) != 0)) {
                st.screen = SCR_BT;
                st.sel    = st.bt_sel;
                st.scroll = st.bt_scroll;
            }
            if (st.screen == SCR_BT) {
                int map[BT_MAX_SCAN_RESULTS];
                int vn = bt_view(&st, map);
                if (st.sel >= vn)
                    st.sel = vn > 0 ? vn - 1 : 0;
                adjust_scroll(&st, vn);
            }
            /* One column per scan window; a gap (0) if the device was not
             * heard recently, rather than repeating the stale reading. */
            if (s_bt_scan_end && st.screen != SCR_BT &&
                st.bt_detail_idx >= 0 && st.bt_detail_idx < bcount) {
                s_bt_rssi_history[s_bt_graph_head] = s_bt_seen[st.bt_detail_idx]
                    ? s_bt_cache[st.bt_detail_idx].rssi : 0;
                s_bt_graph_head = (s_bt_graph_head + 1) % (int)DISP_WIDTH;
            }
            s_bt_scan_end = false;
            st.dirty = true;
        }

        if (s_new_bt_data &&
            (st.screen == SCR_BT_PAIRS || st.screen == SCR_BT_PAIR)) {
            s_new_bt_data = false;
            s_bt_scan_end = false;
            corr_build();
            /* Either device of the open pair left the cache: back to the
             * list rather than show a pair that no longer exists. */
            if (st.screen == SCR_BT_PAIR &&
                (bt_find(st.pair_addr[0]) < 0 || bt_find(st.pair_addr[1]) < 0)) {
                st.screen = SCR_BT_PAIRS;
                st.sel    = st.pair_sel;
                st.scroll = st.pair_scroll;
            }
            if (st.screen == SCR_BT_PAIRS) {
                if (st.sel >= s_pair_count)
                    st.sel = s_pair_count > 0 ? s_pair_count - 1 : 0;
                adjust_pair_scroll(&st);
            }
            st.dirty = true;
        }

        uint8_t btns    = 0u;
        dev_ioctl(DEV_DISPLAY, IOCTL_DISP_GET_BTNS, &btns);
        uint8_t pressed = (uint8_t)(btns & ~prev_btns);  /* rising-edge detect */
        prev_btns = btns;
#ifdef NETMON_AUTOTEST  /* TEMP: scripted presses, one step per 50 loops (~1 s+) */
        {
            static const uint8_t script[] = {
                0, 0, DISP_BTN_X, 0, 0, 0, 0, 0, 0, 0, 0, 0, DISP_BTN_X, 0, 0, 0, 0, 0,
                DISP_BTN_Y, 0, 0, 0, 0, DISP_BTN_Y, 0, DISP_BTN_B, DISP_BTN_X, 0, 0, 0,
                0, 0, 0, 0, 0, DISP_BTN_X, 0, 0, 0, 0, 0, DISP_BTN_Y, 0, 0, DISP_BTN_Y,
                0, DISP_BTN_A,
            };
            static unsigned tick;
            if (tick % 50u == 0u) {
                uint8_t k = script[(tick / 50u) % sizeof(script)];
                if (k) { pressed |= k; printf("[autotest] press %u scr %d\r\n", k, (int)st.screen); }
            }
            tick++;
        }
#endif

        handle_input(&st, pressed);

        /* A menu → scan screen transition happened without lk; take the new
         * screen's lock before rendering its cache. */
        kmutex_t *rlk = screen_lock(st.screen);
        if (rlk != lk) {
            if (lk) kmutex_unlock(lk);
            lk = rlk;
            if (lk) kmutex_lock(lk);
        }

        uint32_t now = now_ms();
        if (st.screen == SCR_WATCH && now - last_tick >= 1000u) {
            last_tick = now;       /* status and event ages */
            st.dirty  = true;
        }

        if (st.dirty) {
            render_screen(&st);
            st.dirty = false;
        }

        if (lk) kmutex_unlock(lk);

        /* ---- no lock held below ---- */

        /* Watch events: report each on the console and flash the LED,
         * green for an arrival, red for a departure. */
        if (s_watch_seq != seen_seq) {
            kmutex_lock(&s_watch_lock);
            uint32_t seq = s_watch_seq;     /* stable while we hold the lock */
            uint32_t n   = seq - seen_seq;
            if (n > WATCH_EVENTS) n = WATCH_EVENTS;
            for (uint32_t k = n; k > 0u; k--) {
                const watch_event_t *e = &s_watch_ev[
                    (s_watch_ev_head - (int)k + 2 * WATCH_EVENTS) % WATCH_EVENTS];
                printf("[netmon] watch: %s %s\r\n", e->name,
                       e->arrived ? "arrived" : "left");
#ifdef PICOOS_LED_ENABLE
                flash_rgb = e->arrived ? 0x00FF00u : 0xFF0000u;
#endif
            }
            kmutex_unlock(&s_watch_lock);
            seen_seq = seq;
#ifdef PICOOS_LED_ENABLE
            flash_until = now + 2000u;
#endif
            if (st.screen == SCR_WATCH) st.dirty = true;
        }

        if (s_watch_dirty && (!save_wait || now - save_retry >= 5000u)) {
            s_watch_dirty = false;
            save_wait     = !watch_save();
            if (save_wait) {
                s_watch_dirty = true;          /* fs busy: try again later */
                save_retry    = now;
            }
        }

        if (s_log_flush || now - last_save >= LOG_SAVE_MS) {
            log_save();
            s_log_flush = false;
            last_save   = now;
        }

#ifdef PICOOS_LED_ENABLE
        uint32_t want_rgb = 0u;
        if ((int32_t)(flash_until - now) > 0)
            want_rgb = ((now / 250u) & 1u) ? flash_rgb : 0u;
        else if (st.screen == SCR_BT_FINDER)
            want_rgb = s_finder_rgb;
        if (want_rgb != led_rgb) {
            led_rgb = want_rgb;
            if (want_rgb) dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &want_rgb);
            else          dev_ioctl(DEV_LED, IOCTL_LED_OFF, NULL);
        }
#endif

        sys_sleep(20);
    }
}
