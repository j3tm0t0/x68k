/*
 * idlebench.x -- CPU が何割残っているかを測る (常駐プログラムが空回りしていないかの確認用)
 *   dbra の空ループ (1 周 10 サイクル) を 3 秒間回し、IOCS _ONTIME (1/100 秒) で測って「MHz 換算」を出す。
 *   常駐物が無ければ CPU のクロックに近い値、常駐スレッドが時間を食うほど小さくなる。
 * start.S を使い回すので入口の名前は zmux_main。
 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdint.h>

extern char *mem_ptr;
extern unsigned char *cmd_line;

static void spin(void)                  /* 65536 周 = 655360 サイクル */
{
  __asm__ __volatile__("move.w #0xffff,%%d0\n1:\tdbra %%d0,1b" ::: "d0", "cc");
}

static void putnum(unsigned long v)
{
  char b[12], *p = b + 11;
  *p = 0;
  do { *--p = '0' + v % 10; v /= 10; } while (v);
  _dos_print(p);
}

static long cs(void)
{
  struct iocs_time t = _iocs_ontime();
  return t.sec;                         /* 0 時からの 1/100 秒 */
}

int zmux_main(void)
{
  long t0, t, el;
  unsigned long chunks = 0, mhz10;
  t0 = cs();
  do {
    spin();
    chunks++;
    t = cs();
    el = t - t0;
    if (el < 0) el += 8640000;
  } while (el < 300);
  mhz10 = chunks * 655360UL / ((unsigned long)el * 1000UL);   /* サイクル / (1/100 秒 * 10000) * 10 */
  _dos_print("IDLEBENCH mhz10=");
  putnum(mhz10);
  _dos_print(" chunks=");
  putnum(chunks);
  _dos_print(" cs=");
  putnum(el);
  _dos_print("\r\n");
  return 0;
}
