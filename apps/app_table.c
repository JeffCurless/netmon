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

#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */
#include "netmon.h"

const app_entry_t app_table[] = {
    { "netmon", netmon_entry, 4u, 0u },   /* name, entry, priority, stack (0 = 2 KB default) */
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));

