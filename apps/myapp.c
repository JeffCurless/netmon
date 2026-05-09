#include "kernel/syscall.h"
#include "shell/shell.h"

void myapp_entry(void *arg)
{
    (void)arg;
    shell_println("myapp: started");

    for (;;) {
        shell_println("myapp: tick");
        sys_sleep(1000);     /* sleep 1 second */
    }
}
