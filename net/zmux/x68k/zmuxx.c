/*
 * zmux.x -- zmux.sys の線を回すスレッドを作って常駐する (net/zmux/DESIGN.md)
 *
 *   zmux.x      常駐してスレッド "zmux" を作る。スレッドは zmux.sys の入口 (命令 0) を呼んでは _CHANGE_PR で譲る
 *   zmux.x -s   統計を表示する
 *   必要: CONFIG.SYS の DEVICE = \SYS\ZMUX.SYS と PROCESS 指定
 * AUX を読み書きしている間は zmux.sys 自身も線を回すので、このスレッドが無くても CTTY AUX は動く。
 * スレッドは、COMMAND.X が別のプログラムを動かしていて AUX を読まない間も、(UP のときだけ) 線の確認と送り直しを続ける。
 */
#define __DOS_INLINE__
#include <x68k/dos.h>
#include <stdint.h>

extern long call_entry(void *entry, long cmd, void *arg);
extern char *mem_ptr;
extern unsigned char *cmd_line;

#define IDLE_SLEEP 1                    /* 1/100 秒単位 */
#define IDLE_SLEEP_MAX 3
#define IDLE_BACKOFF 100                /* 何も無い回がこれだけ続いたら長い方 */
static void *entry;
static struct dos_prcctrl prc;
static unsigned char mbox[4];
static char ustack[2048] __attribute__((aligned(4)));
static char sstack[8192] __attribute__((aligned(4)));

static int memeq(const void *a, const void *b, int n)
{
  const uint8_t *x = a, *y = b;
  while (n--)
    if (*x++ != *y++)
      return 0;
  return 1;
}

/* デバイスチェインから "AUX     " + "ZMUX" を探す。入口はヘッダ +0x1a */
static void *find_zmux(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "AUX     ", 8) && memeq(d + 22, "ZMUX", 4))
          return d + 26;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* contee.sys (画面の出力を RS-232C に写す) が居れば止める。zmux が線を使い始めた後に素の文字が混ざると枠が壊れる */
static void stop_contee(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "\001CONTEE ", 8) || memeq(d + 14, "CONTEE  ", 8))   /* 古い contee.sys の名前も */
          d[22] = 0;
        d = *(uint8_t **)d;
      }
      return;
    }
}

static void thread(void)
{
  int idle = 0;
  for (;;) {
    long r = call_entry(entry, 0, 0);
    /* 仕事が無かった (線が UP でない / 何も来ず何も出ず) なら眠る。譲るだけだと毎回自分の番が回ってきて、
       待ちの間も CPU を食う。眠るのは 10ms、約 1 秒続けて何も無ければ 30ms まで延ばす。仕事があれば元に戻す。
       眠っても AUX を読む側 (COMMAND.X) は自分で線を回すので、対話の遅れには響かない */
    if (r & 2) {
      idle = 0;
      _dos_change_pr();
    } else {
      if (idle < 1000) idle++;
      _dos_sleep_pr(idle >= IDLE_BACKOFF ? IDLE_SLEEP_MAX : IDLE_SLEEP);
    }
  }
}

static void putnum(unsigned long v)
{
  char b[12], *p = b + 11;
  *p = 0;
  do { *--p = '0' + v % 10; v /= 10; } while (v);
  _dos_print(p);
}

int zmux_main(void)
{
  int ssp = _dos_super(0);
  unsigned char *a = cmd_line + 1;
  int i;
  entry = find_zmux();
  if (entry) stop_contee();
  if (ssp > 0) _dos_super(ssp);
  if (!entry) {
    _dos_print("zmux: zmux.sys (AUX) が組み込まれていません\r\n");
    return 1;
  }
  while (*a == ' ') a++;
  if (a[0] == '-' && (a[1] | 0x20) == 'g') {        /* 78125bps (SCC の時定数 0) の見張りを今の SCC の値で入れ直す */
    ssp = _dos_super(0);
    i = call_entry(entry, 9, 0);
    if (ssp > 0) _dos_super(ssp);
    _dos_print(i ? "zmux: SCC time constant guard on\r\n" : "zmux: SCC time constant guard off\r\n");
    return 0;
  }
  if (a[0] == '-' && (a[1] | 0x20) == 's') {
    static const char *nm[] = { "rx frames", "tx frames", "bad", "resend", "raw in", "raw out", "up", "down", "polls" };
    uint32_t *s;
    int i;
    ssp = _dos_super(0);
    s = (uint32_t *)(call_entry(entry, 1, 0) + 8);
    for (i = 0; i < 9; i++) {
      uint32_t v = s[i];
      if (ssp > 0) _dos_super(ssp);
      _dos_print(nm[i]); _dos_print(" "); putnum(v); _dos_print("\r\n");
      ssp = _dos_super(0);
    }
    ssp = _dos_super(0);
    i = s[22];
    if (ssp > 0) _dos_super(ssp);
    _dos_print("thread calls "); putnum(i); _dos_print("\r\n");
    ssp = _dos_super(0);
    i = s[23];
    if (ssp > 0) _dos_super(ssp);
    _dos_print("tc fixed "); putnum(i); _dos_print("\r\n");
    return 0;
  }
  prc.length = sizeof mbox;
  prc.buf_ptr = mbox;
  prc.your_id = 0xffff;
  {
    /* 回数 (2 番目) は Z-OS のペインのスレッドと同じ 10。2 だと Z-OS が動いている間に回ってくる間隔が 2 秒を超えた */
    int tid = _dos_open_pr("zmux", 10, (int)(ustack + sizeof ustack), (int)(sstack + sizeof sstack),
                           0x2000, (int)thread, &prc, 0);
    static char msg[4];
    static struct dos_prcptr me;
    int myid, r;
    if (tid < 0) {
      _dos_print(tid == -27 ? "zmux: 常駐済み\r\n" : "zmux: スレッドを作れません (CONFIG.SYS の PROCESS を確認)\r\n");
      return 1;
    }
    myid = _dos_get_pr(-1, &me);
    while ((r = _dos_send_pr(myid, tid, 0, msg, sizeof msg)) == -28)
      _dos_change_pr();
  }
  _dos_print("zmux: thread started\r\n");
  {
    extern char _end;
    _dos_keeppr(&_end - (mem_ptr + 0x100), 0);
  }
  return 0;
}
