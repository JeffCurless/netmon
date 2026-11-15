#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */
#include "netmon.h"

const app_entry_t app_table[] = {
    { "netmon", netmon_entry, 4u, 0u },   /* name, entry, priority, stack (0 = 2 KB default) */
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));

