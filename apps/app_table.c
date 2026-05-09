#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */
#include "myapp.h"
#include "netmon.h"

const app_entry_t app_table[] = {
    { "myapp",  myapp_entry,  4u },
    { "netmon", netmon_entry, 4u },
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));

