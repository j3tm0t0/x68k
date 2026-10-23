/* zcap.c -- 採取用 (Delta の USB 試験台): TinyUSB のログ (CFG_TUSB_DEBUG=2) を 1 行ずつまとめ、行頭に起動からの ms を付ける。
   UART (115200) は 1 行に 3 ms ほどかかり Z への返事を遅らせるので、毎回出る決まりきった行 (Queue EP / Xfer Complete /
   MSC xfer callback / SCSI Data / SCSI Status = 0) は出さない */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

static char line[200];
static int len;
static uint32_t t0;

static int boring(const char *s)
{
    while (*s == ' ')
        s++;
    static const char *const pre[] = { "Queue EP", "USBD Xfer Complete", "MSC xfer callback", "SCSI Data",
                                       "SCSI Status [Lun0] = 0", "SCSI Status [Lun1] = 0", "Allocated" };
    for (unsigned i = 0; i < sizeof pre / sizeof pre[0]; i++)
        if (!strncmp(s, pre[i], strlen(pre[i])))
            return 1;
    return *s == 0;
}

int zcap_printf(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1)
        n = sizeof b - 1;
    for (int i = 0; i < n; i++) {
        if (len == 0)
            t0 = to_ms_since_boot(get_absolute_time());
        if (b[i] == '\n' || len == (int)sizeof line - 1) {
            line[len] = 0;
            if (!boring(line))
                printf("[%lu] %s\n", (unsigned long)t0, line);
            len = 0;
        } else {
            line[len++] = b[i];
        }
    }
    return n;
}
