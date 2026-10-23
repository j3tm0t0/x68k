/*
 * passtest.c -- パスワードの入力 (x68k_pass.c) のホスト試験。DOS の _INPOUT / _BREAKCK / _CHANGE_PR と fd 2 への出力を真似て
 * x68k_pass.c をそのまま動かす。tests/passtest.sh が組んで流す。
 * 確かめること: エコー無しで画面にはプロンプトと改行だけ、BS / DEL / Ctrl-U、ESC / Ctrl-C で NULL (バッファも消える)、
 *   カーソルキー (KEY.SYS の ^E ^X ^S ^D) などの制御文字は捨てる、長すぎる入力は 255 字で止める、エコー付き (keyboard-interactive の
 *   echo=1) は字と "\b \b" を出す、キーが無い間は _CHANGE_PR で譲る、_BREAKCK は入力中だけ 0 で終われば元に戻る。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- IOCS / DOS の真似 ---- */
static int kq[1024], kn, kr, gap, gapleft;   /* キー (スキャン<<8 | ASCII、届くのは ASCII)。各キーの前に gap 回「空」を返す */
static int _dos_inpout(int code)   /* 0xFF: 待たずに 1 字 (無ければ 0)。キーは ASCII だけが届く */
{
  if (code != 0xff) { fprintf(stderr, "FAIL: _INPOUT(%#x)\n", code); exit(2); }
  if (kr >= kn) { fprintf(stderr, "FAIL: キーが尽きた (入力が終わらない)\n"); exit(2); }
  if (gapleft > 0) { gapleft--; return 0; }
  gapleft = gap;
  return kq[kr++] & 0xff;
}
static int brkflag = 1, brk_during = -1, changes;
static int _dos_breakck(int f) { int o = brkflag; if (f >= 0) brkflag = f; return o; }
static void _dos_change_pr(void) { changes++; if (brk_during < 0) brk_during = brkflag; }
static char out[4096]; static int outn;
static int test_write(int fd, const void *p, int n)
{
  if (fd != 2) { fprintf(stderr, "FAIL: fd %d に書いた\n", fd); exit(2); }
  memcpy(out + outn, p, n); outn += n; out[outn] = 0;
  return n;
}
#define write test_write
#include "../x68k_pass.c"
#undef write

/* ---- 試験の道具 ---- */
static int fails, cases;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
static void reset(int g) { kn = kr = 0; gap = gapleft = g; outn = 0; out[0] = 0; brkflag = 1; brk_during = -1; changes = 0; cases++; }
static void key(int scan, int asc) { kq[kn++] = (scan << 8) | asc; }
static void type(const char *s) { while (*s) key(0x20, (unsigned char)*s++); }
static int burned(void) { int i; for (i = 0; i < X68K_PASS_MAX; i++) if (x68k_pass_buf[i]) return 0; return 1; }
static const char *P = "test@127.0.0.1's password: ";

int main(void)
{
  char *r;
  char big[400];
  int i;

  /* 素直な入力: エコー無し、画面にはプロンプトと改行だけ */
  reset(3); type("x68k"); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strcmp(r, "x68k") == 0, "got '%s'", r ? r : "(null)");
  CHECK(strcmp(out, "test@127.0.0.1's password: \r\n") == 0, "screen '%s'", out);
  CHECK(changes > 0, "キーが無い間に _CHANGE_PR を呼んでいない");
  CHECK(brk_during == 0, "入力中の _BREAKCK が %d", brk_during);
  CHECK(brkflag == 1, "_BREAKCK が戻っていない (%d)", brkflag);
  CHECK(kr == kn, "キーを読み残した");

  /* BS / DEL で 1 字消す。空での BS は何もしない */
  reset(0); key(0x0f, 0x08); type("abX"); key(0x0f, 0x08); type("cdY"); key(0x37, 0x7f); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strcmp(r, "abcd") == 0, "BS: got '%s'", r ? r : "(null)");
  CHECK(r && r[4] == 0 && r[5] == 0, "消した字がバッファに残っている");
  CHECK(strcmp(out, "test@127.0.0.1's password: \r\n") == 0, "BS で画面に出た '%s'", out);

  /* Ctrl-U で全部消す */
  reset(0); type("wrong"); key(0x16, 0x15); type("right"); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strcmp(r, "right") == 0, "Ctrl-U: got '%s'", r ? r : "(null)");

  /* ESC / Ctrl-C は取り消し: NULL、バッファは空、_BREAKCK は戻る */
  reset(1); type("secret"); key(0x01, 0x1b);
  r = x68k_readpass(P, 0);
  CHECK(r == NULL, "ESC で NULL にならない");
  CHECK(burned(), "ESC の後にバッファが残っている");
  CHECK(brkflag == 1, "ESC の後 _BREAKCK が戻っていない");
  CHECK(strcmp(out, "test@127.0.0.1's password: \r\n") == 0, "ESC の画面 '%s'", out);
  reset(0); type("secret"); key(0x2e, 0x03);
  r = x68k_readpass(P, 0);
  CHECK(r == NULL && burned(), "Ctrl-C で取り消しにならない");

  /* カーソルキー (KEY.SYS の既定で ^E ^X ^S ^D)、TAB などの制御文字は捨てる。8 ビットの字 (半角カナ) は通す */
  reset(0); type("p"); key(0x3c, 0x05); key(0x3b, 0x13); key(0x10, '\t'); type("w"); key(0x5a, 0xb1); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strcmp(r, "pw\xb1") == 0, "特殊キー: got '%s'", r ? r : "(null)");

  /* 前の長い入力が残らない (次の呼び出しで消す) */
  reset(0); type("ab"); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strcmp(r, "ab") == 0 && r[2] == 0 && r[5] == 0, "前の入力が残った '%s'", r ? r : "(null)");

  /* 長すぎる入力は 255 字で止まる */
  reset(0); for (i = 0; i < 399; i++) big[i] = 'a' + i % 26; big[399] = 0; type(big); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && strlen(r) == X68K_PASS_MAX - 1 && memcmp(r, big, X68K_PASS_MAX - 1) == 0, "長い入力: %zu 字", r ? strlen(r) : 0);

  /* エコー付き (keyboard-interactive の echo=1 のプロンプト) */
  reset(0); type("usx"); key(0x0f, 0x08); type("er"); key(0x1d, '\r');
  r = x68k_readpass("Name: ", 1);
  CHECK(r && strcmp(r, "user") == 0, "echo: got '%s'", r ? r : "(null)");
  CHECK(strcmp(out, "Name: usx\b \ber\r\n") == 0, "echo の画面 '%s'", out);

  /* 空のパスワード */
  reset(0); key(0x1d, '\r');
  r = x68k_readpass(P, 0);
  CHECK(r && r[0] == 0, "空: got '%s'", r ? r : "(null)");

  if (fails) { fprintf(stderr, "passtest: %d FAIL (%d cases)\n", fails, cases); return 1; }
  printf("passtest: %d cases PASS\n", cases);
  return 0;
}
