/* sst.x: slip.sys / npd.sys の統計 (ETD 9: rx, tx, drop) と slipd.x / npd.x のカウンタを表示する */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <x68k/dos.h>
extern long call_etd(void *entry, long cmd, void *arg);
static void *find_slip(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (!memcmp(d + 14, "/dev/ep0", 8) && (!memcmp(d + 22, "EthDSLIP", 8) || !memcmp(d + 22, "EthDNPD ", 8)))
          return d + 30;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}
int main(void)
{
  uint32_t st[4], *p;
  void *e;
  _dos_super(0);
  e = find_slip();
  if (!e) { printf("no slip.sys / npd.sys\n"); return 1; }
  call_etd(e, 9, st);
  printf("%s rx=%lu tx=%lu drop=%lu ip_handler=%08lx\n", ((char *)e)[-4] == 'N' ? "npd.sys" : "slip.sys", st[0], st[1], st[2], call_etd(e, 6, (long[]){0x0800}));
  for (p = (uint32_t *)0x6800; p < (uint32_t *)0x200000; p++)
    if ((p[0] == 0x534c5044 && (p[1] == 0x53544154 || p[1] == 0x5a535441)) && (void *)p != (void *)main)
      printf("%06lx %.8s %lu %lu %lu %lu\n", (unsigned long)p, (char *)p, p[2], p[3], p[4], p[5]);
  return 0;
}
