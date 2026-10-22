/*
 * ztvctest.c -- 仮想コンソール (ssh.x -Z の Ctrl+F1..F4) のホスト試験。zterm のチャネルを最大 4 本、状態
 * (ztclient / ztdraw / term/vt100 の端末) を持ち替えながら混ぜて流す。ssh.x (net/ssh/dropbear/x68k_shim.c) と同じ手順。
 *   usage: ztvctest [-g CGROM] A.zbin B.zbin [C.zbin D.zbin]   (zterm --replay ... --out で作ったフレーム列)
 *   1. 毎回: 表の VRAM = 表の論理画面の描き直し (vt_host_verify)。裏のチャネルのフレームは VRAM を触らない
 *   2. 各チャネルの ACK の数 = フレームの数 (裏でもフレームを受け取って ACK を返す。サーバは ACK が無いと止まる)
 *   3. 最後: 各チャネルの論理画面が、そのフレーム列だけを 1 本で描いたときと同じ
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ztclient.h"
#include "vt100.h"

extern unsigned char vt_host_cgrom[];
int vt_host_verify(void);

#define NC 4
static void *vst[NC], *scr[NC], *zc[NC], *zd[NC];
static int loaded, shown, n;
static int fails;

static void use(int k)
{
  if (k == loaded) return;
  vt_con_save(vst[loaded]);
  vt_con_load(vst[k]);
  zc_ctx_use(zc[k], 0);
  ztd_ctx_use(zd[k], 0);
  loaded = k;
}
static void show(int k) { use(shown); vt_con_visible(0); use(k); vt_con_visible(1); shown = k; }
static void check(const char *what)
{
  use(shown);
  if (zc_started() && !vt_host_verify()) { fprintf(stderr, "FAIL verify after %s (shown %d)\n", what, shown); fails++; }
}
static long acks(void)   /* zc_output を読み切って 'a' (ACK) を数える */
{
  unsigned char b[512];
  long a = 0;
  int k, i;
  while ((k = zc_output(b, sizeof b)) > 0)
    for (i = 0; i + 1 < k; i += 2) { if (b[i] == 'a') a++; else if (b[i] == 'k') i += b[i + 1]; }
  return a;
}
typedef struct { unsigned short cp; unsigned char fg, bg, fl; } cellv;
static void take(cellv (*s)[96])
{
  int r, c;
  for (r = 0; r < 32; r++) for (c = 0; c < 96; c++) vt_get_cell(r, c, &s[r][c].cp, &s[r][c].fg, &s[r][c].bg, &s[r][c].fl);
}
static unsigned long rnd = 777;
static int rn(int m) { rnd = rnd * 1103515245UL + 12345UL; return (int)((rnd >> 16) % (unsigned long)m); }

int main(int argc, char **argv)
{
  static const char pre[] = "ZTERM OK test\nZTERM GO\n";   /* 起動スクリプトの答え (キャッシュ済み) */
  static cellv snap[NC][32][96], ref[32][96];
  unsigned char *d[NC];
  long len[NC], pos[NC], frames[NC], got[NC];
  int a = 1, k, op = 0;
  const char *name[NC];
  if (a + 1 < argc && !strcmp(argv[a], "-g")) {
    FILE *g = fopen(argv[a + 1], "rb");
    if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cannot read CGROM\n"); return 1; }
    fclose(g); a += 2;
  }
  for (n = 0; a < argc && n < NC; a++, n++) {
    FILE *f = fopen(argv[a], "rb");
    long o;
    if (!f) { perror(argv[a]); return 1; }
    name[n] = argv[a];
    fseek(f, 0, SEEK_END); len[n] = ftell(f) + (long)strlen(pre); fseek(f, 0, SEEK_SET);
    d[n] = malloc(len[n]);
    memcpy(d[n], pre, strlen(pre));
    if (fread(d[n] + strlen(pre), 1, len[n] - strlen(pre), f) != (size_t)(len[n] - strlen(pre))) return 1;
    fclose(f);
    for (frames[n] = 0, o = (long)strlen(pre); o + 3 <= len[n]; o += 3 + ((d[n][o + 1] << 8) | d[n][o + 2])) frames[n]++;
    pos[n] = 0; got[n] = 0;
  }
  if (n < 2) { fprintf(stderr, "usage: ztvctest [-g CGROM] A.zbin B.zbin [C.zbin D.zbin]\n"); return 2; }

  /* チャネル 0 は内蔵の状態 (ssh.x の最初のコンソール)。1.. は作る */
  for (k = 0; k < n; k++) {
    vst[k] = malloc(vt_con_size());
    zc[k] = k ? malloc(zc_ctx_size()) : 0;
    zd[k] = k ? malloc(ztd_ctx_size()) : 0;
  }
  zc_ctx_use(0, 1); ztd_ctx_use(0, 1);
  zc_init("/nonexistent", 96, 32, 0);
  for (;;) {   /* 最初のチャネルの HELLO が画面を占有するまで */
    int m = 64;
    if (m > len[0] - pos[0]) m = (int)(len[0] - pos[0]);
    zc_input(d[0] + pos[0], m); pos[0] += m;
    got[0] += acks();
    if (zc_started() || pos[0] >= len[0]) break;
  }
  if (!zc_started()) { fprintf(stderr, "no hello in %s\n", name[0]); return 1; }
  for (k = 1; k < n; k++) {   /* 新しいコンソール (x68k_shim.c の vc_open) */
    scr[k] = malloc(vt_con_scrsize());
    vt_con_save(vst[loaded]);
    vt_con_new(scr[k]);
    zc_ctx_use(zc[k], 1); ztd_ctx_use(zd[k], 1);
    zc_init("/nonexistent", 96, 32, 0);
    loaded = k;
    if (k == 1) show(1);   /* 作ったコンソールに切り替える (まだ HELLO が来ていない) */
  }
  check("open");

  for (;;) {
    int left = 0, m;
    for (k = 0; k < n; k++) if (pos[k] < len[k]) left = 1;
    if (!left) break;
    do k = rn(n); while (pos[k] >= len[k]);
    m = 1 + rn(rn(3) ? 300 : 4000);
    if (m > len[k] - pos[k]) m = (int)(len[k] - pos[k]);
    use(k);
    zc_input(d[k] + pos[k], m);
    if (zc_state() == ZC_FAIL) { fprintf(stderr, "FAIL %s: %s\n", name[k], zc_error()); return 1; }
    got[k] += acks();
    pos[k] += m;
    check(k == shown ? "frames (shown)" : "frames (hidden)");
    if (++op % 23 == 0) { show(rn(n)); check("switch"); }
    if (op % 41 == 0) { use(shown); vt_con_status(" [1] 2 "); vt_con_status(0); check("status"); }
  }
  for (k = 0; k < n; k++) {
    use(k); take(snap[k]);
    if (got[k] != frames[k]) { fprintf(stderr, "FAIL %s: %ld acks for %ld frames\n", name[k], got[k], frames[k]); fails++; }
  }

  /* 参照: 1 本ずつ */
  for (k = 0; k < n; k++) {
    int r, c, bad = 0;
    vt_fini();
    zc_ctx_use(0, 1); ztd_ctx_use(0, 1);   /* 内蔵の状態を作り直す。画面は HELLO の vt_init が作り直す */
    zc_init("/nonexistent", 96, 32, 0);
    zc_input(d[k], (int)len[k]);
    acks();
    take(ref);
    for (r = 0; r < 32 && !bad; r++)
      for (c = 0; c < 96; c++)
        if (memcmp(&ref[r][c], &snap[k][r][c], sizeof ref[r][c])) {
          fprintf(stderr, "FAIL %s: cell (%d,%d) %04x vs ref %04x\n", name[k], r, c, snap[k][r][c].cp, ref[r][c].cp);
          bad = 1; break;
        }
    fails += bad;
  }
  printf("ztvctest: %d channels, %d chunks, %s\n", n, op, fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
