/* 描画を後回し (vt_defer) にして 1 行ずつ flush し、1 行を 2 回に分けて書いたあと、全部描いた VRAM が全面描き直しと同じかを確かめるホスト試験。
 * 組み方: vt100.c の VT_ZOS 側 sup_enter の move from SR の asm を `sr = 0x2000;` に直した写しと一緒に
 *   cc -O1 -DVT_HOSTTEST -DVT_ZOS -I. -o zosflush tests/zosflush.c vt100_host.c
 * 使い方: zosflush 種 nomove(0/1) [px py pw ph] (枠を与えると zos.x のペインと同じ)。Z 実機の ls -lR の崩れ (行の頭だけ) の調べで使った: 状態のずれは無かった。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"
extern int vt_host_verify(void);
extern int vt_flush_some(int);
extern int vt_defer, vt_nomove;
int main(int argc, char **argv)
{
  int seed = argc > 1 ? atoi(argv[1]) : 1, nomove = argc > 2 ? atoi(argv[2]) : 0, n = 4000, i, bad = 0, w, h;
  srand(seed);
  { int px = argc > 3 ? atoi(argv[3]) : -1, py = argc > 4 ? atoi(argv[4]) : 0, pw = argc > 5 ? atoi(argv[5]) : 96, ph = argc > 6 ? atoi(argv[6]) : 31; extern void vt_host_pane(int,int,int,int); vt_host_pane(px, py, pw, ph); }
  vt_init(96, 32);
  vt_defer = 1; vt_nomove = nomove;
  for (i = 0; i < n; i++) {
    char a[64], b[96];
    int la, lb;
    la = sprintf(a, "-a--rw-   %4d Oct", i % 10000);
    lb = sprintf(b, " 24 16:33 file%04d.txt\r\n", i);
    vt_write((unsigned char *)a, la);
    if (rand() % 3 == 0) vt_flush_some(1 + rand() % 2);
    vt_write((unsigned char *)b, lb);
    if (rand() % 2 == 0) vt_flush_some(1 + rand() % 3);
    if (rand() % 50 == 0) {   /* 全部描いて、全面描き直しと同じか */
      while (vt_flush_some(1)) ;
      if (!vt_host_verify()) { bad++; if (bad < 5) printf("MISMATCH at line %d\n", i); }
    }
  }
  while (vt_flush_some(1)) ;
  printf("seed %d nomove %d: final verify %d, mismatches %d\n", seed, nomove, vt_host_verify(), bad);
  return bad != 0;
}
