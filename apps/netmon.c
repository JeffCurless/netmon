/*
 * netmon.c — NetMon display UI application
 *
 * Screens: Home → Bluetooth (live scan) | WiFi (live scan) | About
 * Buttons: A=up  B=down  X=select  Y=back
 */

#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/sync.h"
#include "kernel/dev.h"
#include "kernel/mem.h"
#include "drivers/display.h"
#include "netmon.h"
#include "kernel/wifi.h"
#include "kernel/bluetooth.h"
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
    int      bt_sel;        /* BT list cursor saved when entering detail panel */
    int      bt_scroll;
    int      bt_detail_idx; /* s_bt_cache index shown in the BT detail panel */
    uint8_t  bt_detail_addr[BT_ADDR_LEN]; /* its address, to spot a reused slot */
    int      detail_scroll; /* first row shown on the BT detail panel */
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

/* Center frequency of a 2.4 GHz channel (the CYW43439 has no 5 GHz radio). */
static int chan_mhz(int ch)
{
    return ch == 14 ? 2484 : 2407 + 5 * ch;
}

/*
 * Build a deduplicated index list from raw scan results.
 * Hidden SSIDs (empty name) and invalid readings (rssi >= 0) are skipped.
 * When the same SSID appears more than once, the entry with the stronger
 * (higher, i.e. less-negative) RSSI is kept, and out_aps[] counts how many
 * BSSIDs carried that SSID.
 * Returns the number of unique entries written to out_idx[] / out_aps[].
 */
static int build_dedup(const wifi_scan_result_t *results, int count,
                       int *out_idx, int *out_aps, int max_out)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (results[i].ssid[0] == '\0') continue;
        /* The CYW43 firmware occasionally reports rssi 0 — not a real
         * reading, and it would beat every genuine one in the max below. */
        if (results[i].rssi >= 0) continue;
        int dup = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(results[i].ssid, results[out_idx[j]].ssid) == 0) {
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

/*
 * Merge a deduplicated scan batch (raw[idx[0..count-1]]) into s_wifi_cache.
 * SSIDs already in the cache take the whole latest reading (from the
 * strongest AP, so BSSID, RSSI and the rest stay consistent) and its channel
 * is added to the list; new SSIDs are appended (up to WIFI_MAX_SCAN_RESULTS).
 * s_wifi_seen[] is rebuilt to mark exactly the entries in this batch.
 */
static void merge_into_cache(const wifi_scan_result_t *raw,
                              const int *idx, const int *aps, int count)
{
    memset(s_wifi_seen, 0, sizeof(s_wifi_seen));
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
            s_wifi_cache[found] = *r;
            s_wifi_aps[found]   = aps[i];
            s_wifi_seen[found]  = true;
            /* Append channel to list if not already present */
            bool ch_seen = false;
            for (int k = 0; k < s_wifi_chan_count[found]; k++) {
                if (s_wifi_chans[found][k] == r->channel) { ch_seen = true; break; }
            }
            if (!ch_seen && s_wifi_chan_count[found] < (int)MAX_CHANS_PER_SSID)
                s_wifi_chans[found][s_wifi_chan_count[found]++] = r->channel;
        } else if (s_wifi_ucount < WIFI_MAX_SCAN_RESULTS) {
            int n = s_wifi_ucount;
            s_wifi_cache[n]       = *r;
            s_wifi_aps[n]         = aps[i];
            s_wifi_chan_count[n]  = 1;
            s_wifi_chans[n][0]   = r->channel;
            s_wifi_seen[n]       = true;
            s_wifi_ucount = n + 1;  /* increment after full write */
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
        draw_message(s_scanning_wifi ? "Scanning..." : "No networks found");
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

static void render_bt(ui_state_t *st)
{
    draw_title("Bluetooth Devices");
    clear_content();

    if (s_bt_count == 0) {
        draw_message(s_scanning_bt ? "Scanning..." : "No devices found");
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

    /* Build every row that has data, then draw the part scrolled into view.
     * Static, not on the stack: the app thread has only DEFAULT_STACK_SIZE
     * and snprintf needs much of it.  Only the UI thread renders. */
    static char    lines[BT_DETAIL_MAX_LINES][20];
    static uint8_t colors[BT_DETAIL_MAX_LINES];
    int     n = 0;
#define LINE(color, ...)                                                    \
    do {                                                                    \
        if (n < BT_DETAIL_MAX_LINES) {                                      \
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

    /* Distance: from the iBeacon's measured power at 1 m if it has one,
     * else from TX power at 0 m (about 41 dB lost in the first metre). */
    bool ibeacon = dev->company_id == 0x004C && dev->mfr_data_len >= 23 &&
                   dev->mfr_data[0] == 0x02 && dev->mfr_data[1] == 0x15;
    int dm = -1;
    if (s_bt_seen[idx] && ibeacon)
        dm = bt_distance_dm((int8_t)dev->mfr_data[22] - dev->rssi);
    else if (s_bt_seen[idx] && dev->tx_power != BT_TX_POWER_UNKNOWN)
        dm = bt_distance_dm(dev->tx_power - dev->rssi - 41);
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
    snprintf(title, sizeof(title), "%.18s", net->ssid);
    draw_title(title);
    clear_content();

    /* Rows are emitted in order; ones with no data are skipped. */
    int  row = 0;
    char tmp[24];

    snprintf(tmp, sizeof(tmp), "RSSI: %ddBm", (int)net->rssi);
    draw_detail_row(row++, tmp, rssi_color(net->rssi));

    if (rssi_range(s_rssi_history, tmp, sizeof(tmp)))
        draw_detail_row(row++, tmp, COLOR_GREY);

    if (net->noise != 0 || net->snr != 0) {
        snprintf(tmp, sizeof(tmp), "Noise:%d SNR:%ddB",
                 (int)net->noise, (int)net->snr);
        draw_detail_row(row++, tmp, COLOR_GREY);
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
    draw_detail_row(row++, tmp, COLOR_GREY);

    if (net->max_rate != 0) {
        const char *std = (net->phy & WIFI_PHY_HT)   ? "11n" :
                          (net->phy & WIFI_PHY_OFDM) ? "11g" : "11b";
        snprintf(tmp, sizeof(tmp), "Mode: %s %s", std,
                 (net->phy & WIFI_PHY_HT40) ? "40MHz" : "20MHz");
        draw_detail_row(row++, tmp, COLOR_GREY);
    }

    if (net->beacon_tu != 0) {
        /* 1 TU = 1.024 ms; rates are in 500 kb/s units. */
        snprintf(tmp, sizeof(tmp), "Bcn:%dms Max:%dM",
                 (int)net->beacon_tu * 1024 / 1000, (int)net->max_rate / 2);
        draw_detail_row(row++, tmp, COLOR_GREY);
    }

    snprintf(tmp, sizeof(tmp), "Sec: %s", auth_label(net->auth_mode));
    draw_detail_row(row++, tmp, COLOR_GREY);

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
    draw_detail_row(row++, tmp, COLOR_GREY);

    /* BSSID of the strongest AP for this SSID */
    const uint8_t *b = net->bssid;
    snprintf(tmp, sizeof(tmp), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    draw_detail_row(row, tmp, COLOR_GREY);

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

/* Lock guarding the scan cache a screen reads, or NULL if it reads none. */
static kmutex_t *screen_lock(screen_t s)
{
    switch (s) {
    case SCR_WIFI: case SCR_WIFI_DETAIL: return &s_wifi_lock;
    case SCR_BT:   case SCR_BT_DETAIL:   return &s_bt_lock;
    default:                             return NULL;
    }
}

static void enter_screen(ui_state_t *st, screen_t s)
{
    st->prev_sel    = st->sel;
    st->prev_scroll = st->scroll;
    st->screen       = s;
    st->sel          = 0;
    st->scroll       = 0;
    st->dirty        = true;

    if (s == SCR_WIFI) {
        kmutex_lock(&s_wifi_lock);
        s_wifi_ucount = 0;
        memset(s_wifi_chan_count, 0, sizeof(s_wifi_chan_count));
        memset(s_wifi_seen, 0, sizeof(s_wifi_seen));
        kmutex_unlock(&s_wifi_lock);
    } else if (s == SCR_BT) {
        kmutex_lock(&s_bt_lock);
        s_bt_count    = 0;
        s_bt_scan_end = false;
        memset(s_bt_seen, 0, sizeof(s_bt_seen));
        memset(s_bt_last_win, 0, sizeof(s_bt_last_win));
        kmutex_unlock(&s_bt_lock);
    }
    s_active_screen = s;
}

static void back_to_home(ui_state_t *st)
{
    screen_t from = st->screen;

    /* Publish HOME before stopping the radio: the stop wakes the worker
     * out of *_scan_wait() at once, and it must then see HOME, not the
     * old screen, or it restarts the scan for another window. */
    s_active_screen = SCR_HOME;
    if (from == SCR_WIFI || from == SCR_WIFI_DETAIL) wifi_scan_stop();
    if (from == SCR_BT   || from == SCR_BT_DETAIL)   bt_scan_stop();

    st->screen = SCR_HOME;
    st->sel    = st->prev_sel;
    st->scroll = st->prev_scroll;
    st->dirty  = true;
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
                memcpy(st->bt_detail_addr, s_bt_cache[st->sel].addr, BT_ADDR_LEN);
                st->detail_scroll = 0;
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
        /* render_bt_detail() clamps the scroll to the rows it has. */
        if (pressed & DISP_BTN_A) { st->detail_scroll--; st->dirty = true; }
        if (pressed & DISP_BTN_B) { st->detail_scroll++; st->dirty = true; }
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

/* ---- scan worker ---------------------------------------------------------- *
 * Runs picoOS continuous scanning for the radio the active screen shows and
 * merges each window into the caches, so the UI thread stays responsive.
 * The window lists come from *_scan_wait() and are ours until the next wait,
 * so they are read with no lock; only the merge into the caches takes
 * s_wifi_lock / s_bt_lock.  s_active_screen is the only lock-free field
 * (single writer, single aligned word).
 *
 * The scan is owned by this process: if netmon is killed, picoOS stops it.  */

typedef enum { RADIO_NONE = 0, RADIO_WIFI, RADIO_BT } radio_t;

static radio_t screen_radio(screen_t s)
{
    switch (s) {
    case SCR_WIFI: case SCR_WIFI_DETAIL: return RADIO_WIFI;
    case SCR_BT:   case SCR_BT_DETAIL:   return RADIO_BT;
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
    merge_into_cache(l->items, idx, aps, n);
    s_scanning_wifi = false;
    s_new_wifi_data = true;
    kmutex_unlock(&s_wifi_lock);
}

static void merge_bt_window(const bt_scan_list_t *l)
{
    kmutex_lock(&s_bt_lock);
    merge_bt_into_cache(l->items, l->count, l->seq);
    s_scanning_bt = false;
    s_new_bt_data = true;
    s_bt_scan_end = true;
    kmutex_unlock(&s_bt_lock);
}

static void scan_worker_entry(void *arg)
{
    (void)arg;
    radio_t running = RADIO_NONE;

    for (;;) {
        radio_t want = screen_radio(s_active_screen);

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
         * when back_to_home() stops the radio. */
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
        if (rc != 0) running = RADIO_NONE;   /* stopped: re-evaluate */
    }
}

/* ---- entry point ---------------------------------------------------------- */

void netmon_entry(void *arg)
{
    (void)arg;

    kmutex_init(&s_wifi_lock);
    kmutex_init(&s_bt_lock);
    s_active_screen = SCR_HOME;

    dev_open(DEV_DISPLAY);

    uint8_t bg = COLOR_PANEL;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

    /* Spawn scan worker to receive WiFi/BT scan windows in the background,
     * keeping the UI thread free for input polling and rendering.
     * Runs AFFINITY_ANY: CYW43 and BTstack run in the async-context IRQ on
     * core 0, so radio calls must not be pinned to C1. */
    pcb_t *proc = task_find_process(sys_getpid());
    task_create_thread(proc, "scan-worker",
                       scan_worker_entry, NULL,
                       5u, DEFAULT_STACK_SIZE);

    ui_state_t st = {
        .screen        = SCR_HOME,
        .sel           = 0,
        .scroll        = 0,
        .prev_sel      = 0,
        .prev_scroll   = 0,
        .wifi_sel      = 0,
        .wifi_scroll   = 0,
        .detail_idx    = 0,
        .bt_sel        = 0,
        .bt_scroll     = 0,
        .bt_detail_idx = 0,
        .detail_scroll = 0,
        .dirty         = true,
    };
    uint8_t prev_btns = 0u;

    for (;;) {
        /* Hold the active screen's cache lock across data intake, input
         * handling and rendering so the worker cannot merge mid-read.
         * Captured up front because handle_input() may change st.screen
         * (enter_screen() takes the target lock itself, from HOME only). */
        kmutex_t *lk = screen_lock(st.screen);
        if (lk) kmutex_lock(lk);

        if (s_new_wifi_data &&
            (st.screen == SCR_WIFI || st.screen == SCR_WIFI_DETAIL)) {
            s_new_wifi_data = false;
            int ucount = s_wifi_ucount;
            if (st.screen == SCR_WIFI) {
                if (st.sel >= ucount)
                    st.sel = ucount > 0 ? ucount - 1 : 0;
                adjust_scroll(&st, ucount);
            }
            if (st.screen == SCR_WIFI_DETAIL &&
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
            (st.screen == SCR_BT || st.screen == SCR_BT_DETAIL)) {
            s_new_bt_data = false;
            int bcount = s_bt_count;
            /* The detail device's slot was reused for another device:
             * go back to the list rather than show the wrong one. */
            if (st.screen == SCR_BT_DETAIL &&
                (st.bt_detail_idx < 0 || st.bt_detail_idx >= bcount ||
                 memcmp(s_bt_cache[st.bt_detail_idx].addr, st.bt_detail_addr,
                        BT_ADDR_LEN) != 0)) {
                st.screen = SCR_BT;
                st.sel    = st.bt_sel;
                st.scroll = st.bt_scroll;
            }
            if (st.screen == SCR_BT) {
                if (st.sel >= bcount)
                    st.sel = bcount > 0 ? bcount - 1 : 0;
                adjust_scroll(&st, bcount);
            }
            /* One column per scan window; a gap (0) if the device was not
             * heard recently, rather than repeating the stale reading. */
            if (s_bt_scan_end && st.screen == SCR_BT_DETAIL &&
                st.bt_detail_idx >= 0 && st.bt_detail_idx < bcount) {
                s_bt_rssi_history[s_bt_graph_head] = s_bt_seen[st.bt_detail_idx]
                    ? s_bt_cache[st.bt_detail_idx].rssi : 0;
                s_bt_graph_head = (s_bt_graph_head + 1) % (int)DISP_WIDTH;
            }
            s_bt_scan_end = false;
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

        /* A HOME → WIFI/BT transition happened without lk; take the new
         * screen's lock before rendering its cache. */
        kmutex_t *rlk = screen_lock(st.screen);
        if (rlk != lk) {
            if (lk) kmutex_unlock(lk);
            lk = rlk;
            if (lk) kmutex_lock(lk);
        }

        if (st.dirty) {
            render_screen(&st);
            st.dirty = false;
        }

        if (lk) kmutex_unlock(lk);

        sys_sleep(20);
    }
}
