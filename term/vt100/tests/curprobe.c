/* curprobe -- 端末 (vt_init 〜 vt_fini) の間に Human68k (IOCS) のカーソルが出ないかを XEiJ で見る試験 (tests/xeij_curprobe.sh)。
   usage: curprobe 結果のファイル
   ssh.x が端末の間にすること (DOS の出力、IOCS の _B_LOCATE / _B_PUTC、キーを見る、スレッドを譲る) のあとに、
   IOCS のワーク $0991 (描かれている)、$0992 (_B_CURON で表示中)、$0993 (_OS_CUROF で禁止) を控えて、最後にファイルへ書く。
   XEiJ の XVI (IPLROM 1.1) でも Hybrid (IPLROM 1.6) でも、_B_PUTC・_B_LOCATE・_CONCTRL などは終わりに _B_CURON を
   呼ぶので、端末が _B_CUROFF で消すだけだと次の出力でまた点滅した (直す前の結果は NG が 4 つ)。
   各段のあと 0.3 秒の間 $0991 を見続ける (表示中なら点滅の最初の 0.05 秒で描かれる)。 */
#include <stdio.h>
#include <string.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include "../vt100.h"

#define NSTEP 20
static char log_[NSTEP][80];
static int nlog, bad;

static void wait_cs(int cs)
{
  int t0 = _iocs_ontime().sec;
  while (_iocs_ontime().sec - t0 < cs) ;
}

static void step(const char *name, int expect_hidden)
{
  int i, s, drawn = 0, on, pro;
  for (i = 0; i < 30; i++) {
    wait_cs(1);
    s = _dos_super(0);
    if (*(volatile unsigned char *)0x991) drawn = 1;
    if (s >= 0) _dos_super(s);
  }
  s = _dos_super(0);
  on = *(volatile unsigned char *)0x992;
  pro = *(volatile unsigned char *)0x993;
  if (s >= 0) _dos_super(s);
  if (expect_hidden && (on || drawn)) bad++;
  if (nlog < NSTEP)
    sprintf(log_[nlog++], "%-12s on=%02x prohibited=%02x drawn=%d%s", name, on, pro, drawn,
            expect_hidden && (on || drawn) ? "  NG" : "");
}

int main(int argc, char **argv)
{
  FILE *f;
  int i, s, on, pro;
  const char *m = "\x1b[2J\x1b[Hcurprobe: the Human68k cursor must not blink while the terminal is up\r\n";
  step("start", 0);
  vt_init(0, 0);
  vt_write((const unsigned char *)m, strlen(m));
  step("vt_init", 1);
  _dos_print("log line from DOS\r\n");        /* ssh.x のログ (fd 2 → CON → _B_PUTC) */
  step("dos_print", 1);
  vt_write((const unsigned char *)"x", 1);
  step("vt_write", 1);
  _iocs_b_locate(0, 3);
  step("b_locate", 1);
  _iocs_b_putc('!');
  step("b_putc", 1);
  _dos_c_fnkmod(3);                           /* _CONCTRL の画面操作 */
  step("c_fnkmod", 1);
  _iocs_b_keysns();                           /* 待つ間にすること: キーを見る (vt100)、DOS の 1 字入力 (x68k_pass.c)、譲る */
  _dos_inpout(0xff);
  _dos_change_pr();
  step("idle", 1);
  _dos_c_curon();                             /* 誰かが禁止を解いても (_CONCTRL 17 = _OS_CURON)、次に描くとき止め直す */
  vt_write((const unsigned char *)"y", 1);
  step("c_curon+wr", 1);
  vt_fini();
  step("vt_fini", 0);
  s = _dos_super(0);
  on = *(volatile unsigned char *)0x992;
  pro = *(volatile unsigned char *)0x993;
  if (s >= 0) _dos_super(s);
  if (!on || pro) bad++;   /* 終わったら COMMAND.X のときの状態 (表示中・禁止なし) に戻っていること */
  f = fopen(argc > 1 ? argv[1] : "CURPROBE.TXT", "w");
  if (!f) return 2;
  for (i = 0; i < nlog; i++) fprintf(f, "%s\r\n", log_[i]);
  fprintf(f, "%s\r\n", bad ? "FAIL" : "PASS");
  fclose(f);
  return bad ? 1 : 0;
}
