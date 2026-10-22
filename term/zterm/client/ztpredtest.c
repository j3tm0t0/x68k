/*
 * ztpredtest.c -- 予測エコー (ztdraw.c の pr_*) の決まった筋書きの試験 (ホスト版)。
 * フレームを手で組んで ztd_frame に、キーを ztd_predict に渡し、論理画面 (予測を重ねた後) を確かめる。
 *   エコーする行 (影の予測 -> 当たって信用 -> 下線で先に描く -> 確定で下線が消える)
 *   エコーしない (パスワード): 外れで取り消し、信用を落とし、以後は描かない
 *   Enter で信用が白紙に・前の epoch の当たりでは信用が戻らない
 *   BS で自分の予測を消す / 行末の BS / ←→ / スクロールで予測を捨てる
 */
#include <stdio.h>
#include <string.h>
#include "ztclient.h"
#include "vt100.h"

static unsigned char fb[512];
static int fl;
static unsigned long kseq;
static int fails;

static void f_begin(void) { fl = 0; }
static void f_byte(int b) { fb[fl++] = (unsigned char)b; }
static void f_text(int r, int c, const char *s) { f_byte(0x02); f_byte(r); f_byte(c); while (*s) f_byte(*s++); }
static void f_cursor(int x, int y) { f_byte(0x09); f_byte(x); f_byte(y); f_byte(1); }
static void f_ack(unsigned long n) { f_byte(0x0f); f_byte((int)(n >> 24) & 255); f_byte((int)(n >> 16) & 255); f_byte((int)(n >> 8) & 255); f_byte((int)n & 255); }
static void f_end(void) { if (ztd_frame(fb, fl) < 0) { printf("FAIL bad frame\n"); fails++; } }
static void keys(const char *s) { int n = (int)strlen(s); kseq += (unsigned long)n; ztd_predict((const unsigned char *)s, n, kseq); }

/* 行 r の桁 c から期待の字 (s) と下線 (u: '_' 下線あり '.' なし) */
static void expect(const char *what, int r, int c, const char *s, const char *u, int cx_want)
{
  int i, x, y, v, ok = 1;
  char got[100], gu[100];
  for (i = 0; s[i]; i++) {
    unsigned short cp; unsigned char fg, bg, f;
    vt_get_cell(r, c + i, &cp, &fg, &bg, &f);
    got[i] = (char)cp; gu[i] = (f & 1) ? '_' : '.';
    if (cp != (unsigned char)s[i] || gu[i] != u[i]) ok = 0;
  }
  got[i] = gu[i] = 0;
  ztd_cursor(&x, &y, &v);
  if (cx_want >= 0 && x != cx_want) ok = 0;
  printf("%s %-44s '%s' %s cursor %d\n", ok ? "PASS" : "FAIL", what, got, gu, x);
  if (!ok) { printf("     want '%s' %s cursor %d\n", s, u, cx_want); fails++; }
}

int main(void)
{
  long hit, miss;
  /* HELLO 40x5、プロンプト "$ "、ECHOACK 0 (サーバが予測エコーに対応) */
  f_begin(); f_byte(0x01); f_byte(1); f_byte(40); f_byte(5); f_text(0, 0, "$ "); f_cursor(2, 0); f_ack(0); f_end();

  keys("a");
  expect("1 shadow (not shown yet)", 0, 2, " ", ".", 2);
  f_begin(); f_text(0, 2, "a"); f_cursor(3, 0); f_end();
  expect("1 echoed -> confirmed", 0, 2, "a", ".", 3);
  keys("bc");
  expect("2 trusted: drawn with underline", 0, 2, "abc", ".__", 5);
  f_begin(); f_text(0, 3, "b"); f_cursor(4, 0); f_ack(1); f_end();
  expect("2 b confirmed, c still predicted", 0, 2, "abc", ".._", 5);
  f_begin(); f_text(0, 4, "c"); f_cursor(5, 0); f_ack(3); f_end();
  expect("2 all confirmed", 0, 2, "abc ", "....", 5);

  keys("\x7f");
  expect("3 BS at end of line predicted", 0, 2, "ab ", "...", 4);
  keys("X");
  expect("3 retype over the BS", 0, 2, "abX", ".._", 5);
  keys("\x7f");
  expect("3 BS removes own prediction", 0, 2, "ab ", "...", 4);
  f_begin(); f_text(0, 4, " "); f_cursor(4, 0); f_ack(6); f_end();
  expect("3 server agrees", 0, 2, "ab  ", "....", 4);

  keys("\x1b[D");
  expect("4 left arrow moves the cursor", 0, 2, "ab", "..", 3);
  keys("\x1b[C");
  expect("4 right arrow back", 0, 2, "ab", "..", 4);
  keys("\x1b[C");
  expect("4 right past the text: not predicted", 0, 2, "ab", "..", 4);
  f_begin(); f_cursor(4, 0); f_ack(15); f_end();

  /* 行の途中への挿入と BS (右の字がずれる) */
  keys("\x1b[D");                                   /* "ab|" -> "a|b" */
  keys("Q");
  expect("4b insert mid-line shifts the rest", 0, 2, "aQb", ".__", 4);
  keys("\x7f");
  expect("4b BS mid-line pulls the rest back", 0, 2, "ab ", "._.", 3);
  f_begin(); f_cursor(3, 0); f_ack(20); f_end();     /* サーバ: "ab" のまま、カーソル 3 */
  expect("4b server agrees", 0, 2, "ab", "..", 3);
  keys("\x1b[C");
  f_begin(); f_cursor(4, 0); f_ack(23); f_end();

  /* パスワード: エコーしない */
  keys("\r");
  f_begin(); f_text(1, 0, "pw: "); f_cursor(4, 1); f_ack(24); f_end();
  keys("s");
  expect("5 after Enter: shadow only", 1, 4, " ", ".", 4);
  f_begin(); f_ack(25); f_end();
  keys("ecret");
  expect("5 no echo -> never shown", 1, 4, "     ", ".....", 4);
  f_begin(); f_ack(30); f_end();
  expect("5 still nothing", 1, 4, "     ", ".....", 4);

  /* 信用のある行で、エコーしない字 (外れ) -> 取り消し */
  keys("\r");
  f_begin(); f_text(2, 0, "$ "); f_cursor(2, 2); f_ack(31); f_end();
  keys("q");
  f_begin(); f_text(2, 2, "q"); f_cursor(3, 2); f_ack(32); f_end();
  keys("w");
  expect("6 trusted again on a new echoing line", 2, 2, "qw", "._", 4);
  f_begin(); f_text(2, 2, "Z"); f_cursor(3, 2); f_ack(33); f_end();
  expect("6 mismatch -> rolled back to the server", 2, 2, "Z ", "..", 3);
  keys("e");
  expect("6 after a miss: shadow", 2, 3, " ", ".", 3);

  /* スクロールで予測を捨てる */
  f_begin(); f_text(2, 3, "e"); f_cursor(4, 2); f_ack(34); f_end();
  keys("rt");
  expect("7 trusted", 2, 3, "ert", ".__", -1);
  f_begin(); f_byte(0x08); f_byte(0); f_byte(4); f_byte(1); f_cursor(4, 1); f_end();
  expect("7 scroll drops predictions", 1, 3, "e ", "..", 4);

  /* 遅いアプリ: キーは届いた (ack) が描き直しはまだ -> 消さずに待ち、後で出たら当たり */
  f_begin(); f_text(3, 0, "> "); f_cursor(2, 3); f_end();
  keys("m");
  f_begin(); f_text(3, 2, "m"); f_cursor(3, 3); f_ack(37); f_end();
  keys("no");
  expect("8 trusted on the slow line", 3, 2, "mno", ".__", 5);
  f_begin(); f_ack(39); f_end();
  expect("8 acked but not redrawn yet: kept", 3, 2, "mno", ".__", 5);
  f_begin(); f_text(3, 3, "no"); f_cursor(5, 3); f_end();
  expect("8 redrawn later: confirmed", 3, 2, "mno", "...", 5);

  ztd_echo_stats(&hit, &miss);
  printf("hits %ld misses %ld\n", hit, miss);
  printf("%s\n", fails ? "FAILED" : "ALL PASS");
  return fails ? 1 : 0;
}
