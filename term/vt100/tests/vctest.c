/*
 * vctest.c -- 仮想コンソール (-DVT_VCONS) のホスト試験。
 *   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o vctest tests/vctest.c vt100.c && ./vctest [-g CGROM] fixtures...
 * フィクスチャを 1 つずつ端末 (最大 4) に割り当て、ばらばらの長さで端末を入れ替えながら流し、ときどき表の端末を切り替える。
 *   1. 毎回: 表の VRAM が表の論理画面の描き直しと同じ (vt_host_verify)。裏の端末への vt_write が VRAM を汚さない
 *   2. 最後: 各端末の論理画面・カーソル・応答 (vt_reply) が、そのフィクスチャだけを 1 つの端末で流したときと同じ
 *   3. 状態表示 (vt_con_status) を出して消すと VRAM が元どおり。vt_init 済みの vt_init は裏の端末の VRAM を触らない
 * 端末の持ち替えは ssh.x (net/ssh/dropbear/x68k_shim.c の vc_use) と同じ手順。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"

extern unsigned char vt_host_vram[4 * 0x20000];
extern unsigned char vt_host_cgrom[0xC0000];
int vt_host_verify(void);

#define NC 4
static void *st[NC], *scr[NC];
static int loaded, shown, ncon;
static unsigned char *data[NC], *reply[NC];
static long len[NC], pos[NC], rlen[NC];
static int fails;

static void use(int k)   /* 端末 k を読み込む (x68k_shim.c の vc_use) */
{
  if (k == loaded) return;
  vt_con_save(st[loaded]);
  vt_con_load(st[k]);
  loaded = k;
}
static void show(int k)  /* 表を k に (x68k_shim.c の vc_switch) */
{
  use(shown); vt_con_visible(0);
  use(k); vt_con_visible(1);
  shown = k;
}
static void check(const char *what)
{
  use(shown);
  if (!vt_host_verify()) { fprintf(stderr, "FAIL verify after %s (shown %d)\n", what, shown); fails++; }
}
static void drain(int k)
{
  unsigned char b[256];
  int n;
  while ((n = vt_reply(b, sizeof b)) > 0) { memcpy(reply[k] + rlen[k], b, n); rlen[k] += n; }
}

typedef struct { unsigned short cp; unsigned char fg, bg, fl; } cellv;
static cellv snap[NC][32][96];
static int snapx[NC], snapy[NC];
static void take(cellv (*s)[96], int *x, int *y)
{
  int r, c;
  for (r = 0; r < 32; r++) for (c = 0; c < 96; c++) vt_get_cell(r, c, &s[r][c].cp, &s[r][c].fg, &s[r][c].bg, &s[r][c].fl);
  vt_get_cursor(x, y);
}

static unsigned long rnd = 12345;
static int rn(int n) { rnd = rnd * 1103515245UL + 12345UL; return (int)((rnd >> 16) % (unsigned long)n); }

int main(int argc, char **argv)
{
  int i, k, a = 1, op = 0;
  if (a + 1 < argc && !strcmp(argv[a], "-g")) {
    FILE *g = fopen(argv[a + 1], "rb");
    if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cannot read CGROM\n"); return 1; }
    fclose(g); a += 2;
  }
  for (ncon = 0; a < argc && ncon < NC; a++, ncon++) {
    FILE *f = fopen(argv[a], "rb");
    if (!f) { perror(argv[a]); return 1; }
    fseek(f, 0, SEEK_END); len[ncon] = ftell(f); fseek(f, 0, SEEK_SET);
    data[ncon] = malloc(len[ncon] + 1); reply[ncon] = malloc(len[ncon] + 256);
    if (fread(data[ncon], 1, len[ncon], f) != (size_t)len[ncon]) return 1;
    fclose(f);
  }
  if (ncon < 2) { fprintf(stderr, "usage: vctest [-g CGROM] a.rec b.rec [c.rec d.rec]\n"); return 1; }

  vt_init(96, 32);
  for (k = 0; k < ncon; k++) st[k] = malloc(vt_con_size());
  for (k = 1; k < ncon; k++) {   /* 端末 1.. を作る (裏) */
    scr[k] = malloc(vt_con_scrsize());
    vt_con_save(st[loaded]);
    vt_con_new(scr[k]);
    loaded = k;
    check("new");
  }
  show(0);
  check("show 0");

  /* 流す: 端末をばらばらに選び、1〜1400 バイトずつ。ときどき表を切り替え、状態表示を出し入れする */
  for (;;) {
    int left = 0, n;
    for (k = 0; k < ncon; k++) if (pos[k] < len[k]) left = 1;
    if (!left) break;
    do k = rn(ncon); while (pos[k] >= len[k]);
    n = 1 + rn(rn(4) ? 64 : 1400);
    if (n > len[k] - pos[k]) n = (int)(len[k] - pos[k]);
    use(k);
    vt_write(data[k] + pos[k], n);
    drain(k);
    pos[k] += n;
    check(k == shown ? "write (shown)" : "write (hidden)");
    if (++op % 37 == 0) { int t = rn(ncon); show(t); check("switch"); }
    if (op % 53 == 0) { use(shown); vt_con_status(" 1 [2] 3 "); vt_con_status(0); check("status"); }
  }

  /* vt_init 済みの vt_init: いまの端末 (裏) の状態だけ初期化し、VRAM は触らない */
  {
    static unsigned char before[4 * 0x20000];
    int h = (shown + 1) % ncon, x, y;
    unsigned short cp;
    memcpy(before, vt_host_vram, sizeof before);
    use(h);
    vt_init(96, 32);
    vt_get_cursor(&x, &y); vt_get_cell(0, 0, &cp, 0, 0, 0);
    if (memcmp(before, vt_host_vram, sizeof before)) { fprintf(stderr, "FAIL re-init of hidden console touched VRAM\n"); fails++; }
    if (x || y || cp != ' ') { fprintf(stderr, "FAIL re-init did not reset console %d\n", h); fails++; }
    check("re-init hidden");
    /* 試験 2 のため、その端末には最初から流し直す */
    use(h);
    vt_write(data[h], (int)len[h]);
    rlen[h] = 0; drain(h);
    check("refill");
  }

  for (k = 0; k < ncon; k++) { use(k); take(snap[k], &snapx[k], &snapy[k]); }

  /* 参照: 1 つの端末で 1 つずつ流す */
  for (k = 0; k < ncon; k++) {
    static cellv ref[32][96];
    static unsigned char rb[1 << 16];
    long rl = 0;
    int x, y, r, c, bad = 0, n;
    vt_fini();
    vt_init(96, 32);
    for (i = 0; i < len[k]; i += 1400) {
      vt_write(data[k] + i, (int)(len[k] - i < 1400 ? len[k] - i : 1400));
      while ((n = vt_reply(rb + rl, 256)) > 0) rl += n;
    }
    take(ref, &x, &y);
    for (r = 0; r < 32 && !bad; r++)
      for (c = 0; c < 96; c++)
        if (memcmp(&ref[r][c], &snap[k][r][c], sizeof ref[r][c])) {
          fprintf(stderr, "FAIL %s: cell (%d,%d) %04x/%d/%d/%d vs ref %04x/%d/%d/%d\n", argv[argc - ncon + k], r, c,
                  snap[k][r][c].cp, snap[k][r][c].fg, snap[k][r][c].bg, snap[k][r][c].fl, ref[r][c].cp, ref[r][c].fg, ref[r][c].bg, ref[r][c].fl);
          bad = 1; break;
        }
    if (x != snapx[k] || y != snapy[k]) { fprintf(stderr, "FAIL %s: cursor %d,%d vs ref %d,%d\n", argv[argc - ncon + k], snapx[k], snapy[k], x, y); bad = 1; }
    if (rl != rlen[k] || memcmp(rb, reply[k], rl)) { fprintf(stderr, "FAIL %s: replies %ld bytes vs ref %ld\n", argv[argc - ncon + k], rlen[k], rl); bad = 1; }
    fails += bad;
  }
  printf("vctest: %d consoles, %d writes, %s\n", ncon, op, fails ? "FAIL" : "ok");
  return fails ? 1 : 0;
}
