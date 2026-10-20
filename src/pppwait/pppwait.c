/*
 * pppwait.x -- 相手 (Raspberry Pi の pppd) が PPP で待ち受けているか確かめる
 *
 * ppp.x -dial は相手が応答しないと "Phase: Dead" のまま戻らず、BREAK も効かない。
 * pppd は待受中に数秒おきに LCP Configure-Request (7e ff 7d 23 c0 21 ...) を送ってくるので、
 * それを RS-232C (tmsio.x 常駐後の IOCS) で受信できたら 0 で終了し、ppp.x を起動してよい。
 * ESC / BREAK / Ctrl-C を押すか、タイムアウトすると 1 で終了する。
 *
 *   usage: pppwait.x [秒]      (既定 30 秒)
 *   build: gcc pppwait.c -o pppwait.x -liocs
 */
#include <stdio.h>
#include <stdlib.h>
#include <iocslib.h>

static int abort_key(void)
{
  /* BREAK キー (スキャンコード 0x61 = BITSNS グループ 12 の bit1) */
  if (_iocs_bitsns(12) & 0x02)
    return 1;
  if (_iocs_b_keysns()) {
    int c = _iocs_b_keyinp() & 0xff;
    if (c == 0x1b || c == 0x03)
      return 1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  /* LCP フレームの先頭: flag 7e, addr ff, ctrl 03 (= 7d 23), proto c0 21 */
  static const unsigned char pat[] = { 0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21 };
  int sec = argc > 1 ? atoi(argv[1]) : 30;
  int start = _iocs_ontime(), got = 0, shown = -1;

  printf("pppwait: waiting for PPP peer (%d sec, ESC/BREAK to abort)\n", sec);
  for (;;) {
    int el = (_iocs_ontime() - start) / 100;   /* ontime は 1/100 秒 */
    if (el < 0)
      el += 24 * 60 * 60;
    if (el >= sec) {
      printf("\npppwait: no peer\n");
      return 1;
    }
    if (el != shown) {
      printf("\r%d ", sec - el);
      fflush(stdout);
      shown = el;
    }
    if (abort_key()) {
      printf("\npppwait: aborted\n");
      return 1;
    }
    while (_iocs_isns232c()) {
      unsigned char c = _iocs_inp232c() & 0xff;
      if (c == pat[got])
        got++;
      else
        got = (c == pat[0]) ? 1 : 0;
      if (got == sizeof pat) {
        printf("\npppwait: peer found\n");
        return 0;
      }
    }
  }
}
