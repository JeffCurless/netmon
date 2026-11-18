/*
 * oui.c — MAC address vendor lookup over the generated oui_table[].
 */

#include "oui.h"
#include <stddef.h>

const char *oui_vendor(uint8_t o0, uint8_t o1, uint8_t o2)
{
    if (oui_is_local(o0)) return NULL;
    uint32_t key = ((uint32_t)o0 << 16) | ((uint32_t)o1 << 8) | o2;
    int lo = 0, hi = oui_table_len - 1;
    while (lo <= hi) {
        int      mid = lo + (hi - lo) / 2;
        uint32_t oui = oui_table[mid] >> 8;
        if (oui == key) return oui_names[oui_table[mid] & 0xFFu];
        if (oui < key) lo = mid + 1;
        else           hi = mid - 1;
    }
    return NULL;
}
