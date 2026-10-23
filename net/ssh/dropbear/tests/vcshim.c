/*
 * vcshim.c -- ssh.x の仮想コンソール (x68k_vcons.c) のホスト試験。dropbear の代わりにチャネルの開け閉め・読み書きを、
 * IOCS の代わりにキー (スキャンコード + シフト状態) と時計を真似て、x68k_vcons.c をそのまま動かす。
 *   tests/vcshim.sh が組んで流す (term/vt100 のホスト版 VRAM、term/zterm のサーバが作ったフレーム列)。
 *   usage: vcshim A.zbin B.zbin   (-Z の試験に使う 2 本のフレーム列)
 * 確かめること:
 *   -t: 最初のチャネルがコンソール 1、Ctrl+F2 / OPT.1+F2 で新しいチャネルを (cli_sessionloop で) 開き pty + シェルを頼む、
 *       キーは表のチャネルにだけ、裏の端末の出力は VRAM を変えず DSR の応答はそのチャネルへ、Ctrl-] 1 / Ctrl-] Ctrl-] /
 *       Ctrl-] x、表を閉じると前のコンソールに戻る、最後の 1 つで 0 (画面を戻す)
 *   -Z: 2 つ目のコンソールも zterm (起動スクリプトを exec)、裏でもフレームに ACK を返し VRAM を変えない、
 *       ヘルパが始まらずに閉じたコンソールは同じ番号で -t に開き直す
 *   毎回: 表の VRAM = 表の論理画面を全部描き直した絵 (vt_host_verify)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "vt100.h"
#include "ztclient.h"

/* ---- IOCS / DOS / dropbear の真似 ---- */
#define X68K_SOCKFD_MIN 128
extern unsigned char vt_host_vram[4 * 0x20000];
int vt_host_verify(void);
#define TVRAM_BASE ((volatile unsigned char *)vt_host_vram)
struct iocs_time { int sec, day; };
static int clk;                        /* 1/100 秒 */
static struct iocs_time _iocs_ontime(void) { struct iocs_time t; t.sec = clk; t.day = 0; return t; }
static char con[4096];                 /* IOCS _B_PRINT に出たもの */
static void _iocs_b_print(const char *s) { strncat(con, s, sizeof con - strlen(con) - 1); }
static void _iocs_b_locate(int x, int y) { (void)x; (void)y; }
static void _iocs_b_down_s(void) {}
static void _iocs_txrascpy(int a, int b, int c) { (void)a; (void)b; (void)c; }
static int _dos_super(int x) { (void)x; return -1; }
static void x68dbg(const char *m) { (void)m; }
static void dropbear_exit(const char *f, ...) { fprintf(stderr, "dropbear_exit: %s\n", f); exit(3); }
/* キー: スキャンコード<<8 | ASCII と、そのときの _B_SFTSNS */
static int kq[64], ks[64], kn, kr;
static int cur_sft;
int vt_host_keysns(void) { return kr < kn ? kq[kr] : 0; }
int vt_host_keyinp(void) { if (kr >= kn) return 0; cur_sft = ks[kr]; return kq[kr++]; }
static int _iocs_b_keysns(void) { return vt_host_keysns(); }
static int _iocs_b_keyinp(void) { return vt_host_keyinp(); }
static int _iocs_b_sftsns(void) { return cur_sft; }   /* 最後に読んだキーを押したときのシフト状態 */
static void key(int scan, int asc, int sft) { kq[kn] = (scan << 8) | asc; ks[kn] = sft; kn++; }
static void type(const char *s) { while (*s) key(0x20, (unsigned char)*s++, 0); }
static int nextchan = 1, opened;
int x68k_open_session(void) { opened++; return nextchan++; }

#include "../x68k_vcons.c"

/* ---- 試験の道具 ---- */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
static void settle(void) { clk += 200; kbuf_fill(); }   /* 状態表示が消えるまで進める */
static int verify(void) { vc_use(vc_act); return vt_host_verify(); }
static void row0(char *b, int n)   /* 表の端末の 0 行目 */
{
  int c;
  vc_use(vc_act);
  for (c = 0; c < n; c++) { unsigned short cp; vt_get_cell(0, c, &cp, 0, 0, 0); b[c] = cp < 0x7f ? (char)cp : '?'; }
  b[n] = 0;
}
static void w(int chan, const char *s) { CHECK(x68k_chan_write(chan, 1, s, (unsigned)strlen(s)) == (int)strlen(s), "write"); }
static int rd(int chan, char *b, int n) { int r = x68k_chan_read(chan, 0, b, (unsigned)n); if (r < 0) { CHECK(errno == EINTR, "errno"); r = 0; } b[r] = 0; return r; }
static int rd_all(int chan, char *b, int n) { int k = 0, i; for (i = 0; i < 8; i++) k += rd(chan, b + k, n - k); b[k] = 0; return k; }   /* 前置キーは 1 回の読みで 1 つずつ進む */
static void opench(int k, int *pty, char **cmd)   /* x68k_vc_loop が開いたチャネルの確認が来た */
{
  *pty = -1; *cmd = (char *)"unset";
  x68k_vc_loop();
  CHECK(vc[k].chan > 0, "console %d not opened", k + 1);
  x68k_vc_initchan(vc[k].chan, pty, cmd);
}

static unsigned char *slurp(const char *f, long *n)
{
  FILE *fp = fopen(f, "rb");
  unsigned char *d;
  if (!fp) { perror(f); exit(1); }
  fseek(fp, 0, SEEK_END); *n = ftell(fp); fseek(fp, 0, SEEK_SET);
  d = malloc(*n + 32);
  if (fread(d, 1, *n, fp) != (size_t)*n) exit(1);
  fclose(fp);
  return d;
}
static long acks(int chan)   /* チャネルの stdin に出た zterm の ACK を数える */
{
  unsigned char b[4096];
  long a = 0;
  int r, i;
  while ((r = x68k_chan_read(chan, 0, b, sizeof b)) > 0)
    for (i = 0; i + 1 < r; i += 2) { if (b[i] == 'a') a++; else if (b[i] == 'k') i += b[i + 1]; }
  return a;
}
static long frames(const unsigned char *d, long n) { long o = 0, k = 0; while (o + 3 <= n) { o += 3 + ((d[o + 1] << 8) | d[o + 2]); k++; } return k; }
static long zfeed(int chan, const unsigned char *d, long n, long step)   /* フレーム列を step バイトずつチャネルの stdout へ。ACK の数を返す */
{
  long o, a = 0;
  for (o = 0; o < n; o += step) { x68k_chan_write(chan, 1, d + o, (unsigned)(n - o < step ? n - o : step)); a += acks(chan); }
  return a;
}

static void test_t(void)
{
  int pty, r;
  char *cmd, b[256];
  /* 最初のチャネル: cli_opts のまま */
  pty = 7; cmd = (char *)"orig";
  x68k_vc_initchan(0, &pty, &cmd);
  CHECK(pty == 7 && !strcmp(cmd, "orig"), "first channel must keep cli_opts");
  x68k_tty_setup();
  w(0, "console one\r\n$ ");
  type("ls\r");
  r = rd_all(0, b, sizeof b);
  CHECK(r == 3 && !memcmp(b, "ls\r", 3), "keys to console 1: got %d '%s'", r, b);
  CHECK(verify(), "verify 1");

  /* Ctrl+F2: コンソール 2 を作る (チャネルは cli_sessionloop で) */
  key(0x64, 0, 0x02);
  r = rd(0, b, sizeof b);
  CHECK(r == 0 && vc_act == 1 && vc[1].used && vc[1].chan == -1 && vc_wake, "ctrl+f2 must open console 2 (act %d)", vc_act);
  type("x");                                   /* チャネルが開く前の打鍵は溜めておく */
  kbuf_fill();
  CHECK(!in_ready(), "keys for an unopened channel must not wake select");
  opench(1, &pty, &cmd);
  CHECK(pty == 1 && cmd == 0, "console 2 must ask pty + shell (pty %d)", pty);
  CHECK(!vc_wake && opened == 1, "opened once");
  r = rd_all(vc[1].chan, b, sizeof b);
  CHECK(r == 1 && b[0] == 'x', "early key goes to console 2");
  settle();
  row0(b, 11);
  CHECK(!strcmp(b, "           "), "new console blank: '%s'", b);
  CHECK(verify(), "verify blank console 2");
  w(vc[1].chan, "console two\r\n# ");
  row0(b, 11);
  CHECK(!strcmp(b, "console two"), "console 2 shown: '%s'", b);
  CHECK(verify(), "verify console 2");

  /* 裏のコンソール 1 への出力: VRAM はそのまま、DSR の応答はコンソール 1 のチャネルへ */
  {
    static unsigned char before[4 * 0x20000];
    memcpy(before, vt_host_vram, sizeof before);
    w(0, "\x1b[6nmore output on one\r\n");
    CHECK(!memcmp(before, vt_host_vram, sizeof before), "hidden write touched VRAM");
    CHECK(verify(), "verify after hidden write");
    CHECK(rd(vc[1].chan, b, sizeof b) == 0, "no reply on console 2");
    r = rd(0, b, sizeof b);
    CHECK(r > 0 && !strcmp(b, "\x1b[2;3R"), "DSR reply on console 1: '%s'", b + 1);
  }
  type("z");
  CHECK(rd(0, b, sizeof b) == 0, "console 1 must not get keys while hidden");
  CHECK(rd(vc[1].chan, b, sizeof b) == 1 && b[0] == 'z', "key to console 2");

  /* Ctrl-] 1: コンソール 1 へ */
  key(0x1b, 0x1d, 0x02); type("1");
  rd(vc[1].chan, b, sizeof b); rd(vc[1].chan, b, sizeof b);
  CHECK(vc_act == 0, "ctrl-] 1 must switch to console 1 (act %d)", vc_act);
  CHECK(rd(0, b, sizeof b) == 0, "prefix keys must not be sent");
  settle();
  row0(b, 11);
  CHECK(!strcmp(b, "console one"), "console 1 back: '%s'", b);
  CHECK(verify(), "verify console 1 back");
  /* Ctrl-] Ctrl-] は 1 つ、Ctrl-] x は両方 */
  key(0x1b, 0x1d, 0x02); key(0x1b, 0x1d, 0x02);
  key(0x1b, 0x1d, 0x02); type("x");
  r = rd_all(0, b, sizeof b);
  CHECK(r == 3 && !memcmp(b, "\x1d\x1dx", 3), "escape literal: %d bytes", r);
  /* OPT.1+F2 でもコンソール 2 へ。修飾無しの F2 は ESC O Q */
  key(0x64, 0, 0x00);
  r = rd_all(0, b, sizeof b);
  CHECK(r == 3 && !memcmp(b, "\x1bOQ", 3) && vc_act == 0, "plain F2 must go to the remote");
  key(0x64, 0, 0x04);
  rd(0, b, sizeof b);
  CHECK(vc_act == 1, "opt.1+f2 must switch");
  settle();
  CHECK(verify(), "verify after opt.1+f2");

  /* Ctrl+F3 でコンソール 3、それを閉じると前 (2) に戻る */
  key(0x65, 0, 0x02);
  rd(vc[1].chan, b, sizeof b);
  CHECK(vc_act == 2, "ctrl+f3");
  opench(2, &pty, &cmd);
  w(vc[2].chan, "three");
  CHECK(x68k_vc_closed(vc[2].chan) == 1, "closing console 3 with others alive");
  CHECK(vc_act == 1 && !vc[2].used, "back to console 2 (act %d)", vc_act);
  settle();
  row0(b, 11);
  CHECK(!strcmp(b, "console two"), "console 2 after closing 3: '%s'", b);
  CHECK(verify(), "verify after close");

  /* サーバがチャネルを開かなかった (MaxSessions など): そのコンソールを捨てて前に戻る */
  key(0x66, 0, 0x02);
  rd(vc[1].chan, b, sizeof b);
  CHECK(vc_act == 3, "ctrl+f4");
  x68k_vc_loop();
  x68k_vc_openfail(vc[3].chan);
  CHECK(!vc[3].used && vc_act == 1, "open failure drops console 4 (act %d)", vc_act);
  settle();
  CHECK(verify(), "verify after open failure");
  /* 裏のコンソール 1 を閉じる: 表はそのまま */
  CHECK(x68k_vc_closed(0) == 1 && vc_act == 1 && !vc[0].used, "close hidden console 1");
  settle();
  CHECK(verify(), "verify after closing hidden");
  /* 最後のコンソール */
  CHECK(x68k_vc_closed(vc[1].chan) == 0, "last console returns 0");
  x68k_tty_cleanup();
  CHECK(!vt_active, "screen restored");
}

static void test_z(const char *fa, const char *fb)
{
  static const char pre[] = "ZTERM OK test\nZTERM GO\n";
  long na, nb, fra, frb, a0, a1;
  unsigned char *da = slurp(fa, &na), *db = slurp(fb, &nb);
  int pty, c1, c2;
  char *cmd, b[256];
  memset(vc, 0, sizeof vc); vc_ld = -1; vc_act = vc_prev = 0; zt_fallback = 0; vt_keyhook = 0;
  fra = frames(da, na); frb = frames(db, nb);
  { char *av[] = { "ssh.x", "-Z", "host", 0 }; int ac = 3; x68k_zterm_args(&ac, av); }
  CHECK(x68k_zterm_command(0) != 0, "-Z command");
  x68k_vc_initchan(10, &pty, &cmd);
  CHECK(vc[0].zt, "console 1 is zterm");
  w(10, pre);
  a0 = zfeed(10, da, na / 2, 333);             /* 前半 */
  CHECK(vt_active, "HELLO occupied the screen");
  CHECK(verify(), "verify z1");

  key(0x64, 0, 0x02);                          /* Ctrl+F2 */
  rd(10, b, sizeof b);
  CHECK(vc_act == 1 && vc[1].zt, "console 2 is zterm too");
  nextchan = 20;
  opench(1, &pty, &cmd);
  c1 = vc[1].chan;
  CHECK(pty == 0 && cmd && !strncmp(cmd, "exec sh -c", 10), "console 2 runs the zterm bootstrap");
  w(c1, pre);
  a1 = zfeed(c1, db, nb, 500);
  CHECK(a1 == frb, "console 2 acks %ld of %ld", a1, frb);
  settle();
  CHECK(verify(), "verify z2");
  {
    static unsigned char before[4 * 0x20000];
    memcpy(before, vt_host_vram, sizeof before);
    a0 += zfeed(10, da + na / 2, na - na / 2, 700);   /* 裏のコンソール 1 の後半 (フレームの途中で切れていてもよい) */
    CHECK(!memcmp(before, vt_host_vram, sizeof before), "hidden zterm frames touched VRAM");
    CHECK(a0 == fra, "hidden console 1 acks %ld of %ld", a0, fra);
  }
  key(0x63, 0, 0x02);                          /* Ctrl+F1 */
  rd(c1, b, sizeof b);
  CHECK(vc_act == 0, "back to console 1");
  settle();
  CHECK(verify(), "verify z1 after hidden frames");

  /* コンソール 3: ヘルパが始まらずに閉じた -> 同じコンソールを -t で開き直す */
  key(0x65, 0, 0x02);
  rd(10, b, sizeof b);
  nextchan = 30;
  opench(2, &pty, &cmd);
  c2 = vc[2].chan;
  w(c2, "ZTERM FAIL nobinary\n");
  CHECK(x68k_vc_closed(c2) == 1 && vc[2].used && !vc[2].zt && vc[2].chan == -1 && vc_wake, "fallback must reopen console 3 as -t");
  opench(2, &pty, &cmd);
  CHECK(pty == 1 && cmd == 0 && vc[2].chan == 31, "console 3 reopened with pty (chan %d)", vc[2].chan);
  w(31, "plain three\r\n");
  settle();
  row0(b, 11);
  CHECK(!strcmp(b, "plain three"), "console 3 as -t: '%s'", b);
  CHECK(verify(), "verify console 3 -t");
  type("q");
  CHECK(rd(31, b, sizeof b) == 1 && b[0] == 'q', "key to -t console 3");
  /* 以後の新しいコンソールも -t */
  key(0x66, 0, 0x02);
  rd(31, b, sizeof b);
  CHECK(vc_act == 3 && !vc[3].zt, "after a fallback new consoles are -t");
  opench(3, &pty, &cmd);
  CHECK(pty == 1, "console 4 pty");
  /* 閉じていく。zterm のコンソールの入れ物を捨てても他の zterm は動く */
  CHECK(x68k_vc_closed(vc[3].chan) == 1, "close 4");
  CHECK(x68k_vc_closed(c1) == 1, "close 2 (zterm, hidden)");
  key(0x63, 0, 0x02);
  rd(31, b, sizeof b);
  CHECK(vc_act == 0, "to console 1");
  vc_zc(0);
  CHECK(zc_state() == ZC_RUN || zc_state() == ZC_DONE, "console 1 zterm state %d", zc_state());
  vc_zc_back();
  settle();
  CHECK(verify(), "verify z1 replay");
  CHECK(x68k_vc_closed(31) == 1, "close 3");
  CHECK(x68k_vc_closed(10) == 0 && !vt_active, "last zterm console restores the screen");
}

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: vcshim A.zbin B.zbin\n"); return 2; }
  test_t();
  test_z(argv[1], argv[2]);
  printf("vcshim: %s\n", fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
