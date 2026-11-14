/* protprobe.x: 68000 + X68000 のハードウェアで、.x をどこまで閉じ込められるかを調べる
 *   1. ユーザーモードから各領域 (VRAM, I/O, SRAM, ROM, 主記憶の下側) を読めるか
 *   2. ユーザーモードからテキスト VRAM に書いたとき、書き込みが本当に止まるか
 *   3. 特権命令 (move to SR / move from SR) がどう扱われるか
 *   4. スーパーバイザ領域設定 $E86001 で主記憶の下側を守れるか (守ったまま DOS / IOCS が使えるか)
 *   5. 増設メモリのスーパーバイザ領域設定 $EAFF81-$EAFF89 (256KB 単位) で守れるか
 *   6. ユーザーモードから守られた所へ jsr (An) したとき、戻り番地を積んでからバスエラーになるか (Z-OS の prot_ti_fault)
 * 例外は trap.S のハンドラで受けて記録し、続きから戻る。最後にベクタと設定を元に戻す。
 * 68000 専用 (バスエラーのフレームの形が 68010 以降と違う)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

extern void buserr_h(void), addrerr_h(void), priv_h(void);
extern int probe_rb(unsigned long a), probe_rw(unsigned long a);
extern int probe_wb(unsigned long a, int v), probe_ww(unsigned long a, int v);
extern int probe_movesr(void), probe_movefromsr(void), probe_jsr(unsigned long a);
extern char insn_jsr[];
extern volatile unsigned short f_frame[7];
extern volatile unsigned long f_usp, f_usptop, f_usp0;
extern char insn_rb[], insn_rw[], insn_wb[], insn_ww[], insn_movesr[];
extern volatile unsigned short f_kind, f_info, f_ir, f_sr;
extern volatile unsigned long f_addr, f_pc;

extern void *old_bus, *old_adr, *old_priv;
static void *old2, *old3, *old8;

/* 割り込みの許可レベルを変える (スーパーバイザに入って SR を書き、戻る)。
 * 守る範囲を広げている間は 7 にして、OS の裏のタスクに切り替わらないようにする */
static void set_ipl(int ipl)
{
  int ssp = _dos_super(0);
  if (ipl) __asm__ volatile("ori.w #0x0700,%%sr" ::: "cc");
  else __asm__ volatile("andi.w #0xf8ff,%%sr" ::: "cc");
  _dos_super(ssp);
}
static int ng;

static const char *kname(int k)
{
  return k == 0 ? "ok" : k == 2 ? "BUSERR" : k == 3 ? "ADRERR" : k == 8 ? "PRIV" : "?";
}

/* スーパーバイザモードで probe を 1 回呼ぶ */
static int sup_rw(unsigned long a)
{
  int ssp = _dos_super(0), r = probe_rw(a);
  _dos_super(ssp);
  return r;
}
static int sup_rb(unsigned long a)
{
  int ssp = _dos_super(0), r = probe_rb(a);
  _dos_super(ssp);
  return r;
}
static int sup_wb(unsigned long a, int v)
{
  int ssp = _dos_super(0), r = probe_wb(a, v);
  _dos_super(ssp);
  return r;
}
static unsigned short sup_peekw(unsigned long a)
{
  int ssp = _dos_super(0);
  unsigned short v = *(volatile unsigned short *)a;
  _dos_super(ssp);
  return v;
}
static void sup_pokew(unsigned long a, unsigned short v)
{
  int ssp = _dos_super(0);
  *(volatile unsigned short *)a = v;
  _dos_super(ssp);
}

/* ユーザーモードで 8KB ずつ読んで、読める最初の番地 (= 守られている範囲の終わり) を返す */
static unsigned long measure(unsigned long lim)
{
  unsigned long a = 0x2000;
  while (a < lim && probe_rw(a) != 0) a += 0x2000;
  return a;
}

/* 期待どおりかを 1 行出す */
static void expect(const char *what, int got, int want)
{
  printf("  %-44s %-6s %s\n", what, kname(got), got == want ? "" : "  <== 期待と違う");
  if (got != want) ng++;
}

/* 直前の例外のフレームの中身 */
static void frame(const char *insn)
{
  printf("    frame: info=%04x addr=%06lx ir=%04x sr=%04x pc=%06lx (命令 %06lx, pc-命令=%ld)\n",
         f_info, f_addr, f_ir, f_sr, f_pc, (unsigned long)insn, (long)(f_pc - (unsigned long)insn));
}

int main(void)
{
  int ssp = _dos_super(0);
  unsigned char mpu = *(volatile unsigned char *)0xcbc;
  unsigned long ram = *(volatile unsigned long *)0xed0008;
  _dos_super(ssp);
  unsigned long low = (unsigned long)main < (unsigned long)probe_rb ? (unsigned long)main : (unsigned long)probe_rb;
  printf("protprobe: MPU=680%d0  RAM=%luKB  このプログラム=%06lx 付近\n", mpu, ram / 1024, low);
  if (mpu != 0) {
    printf("68000 でしか試せない (バスエラーのフレームの形が違う)\n");
    return 1;
  }

  /* 出力は最後までためる (割り込みを止めている間に DOS で書き出さないため) */
  static char obuf[16384];
  setvbuf(stdout, obuf, _IOFBF, sizeof obuf);
  ssp = _dos_super(0);
  old_bus = old2 = *(void **)(2 * 4);
  old_adr = old3 = *(void **)(3 * 4);
  old_priv = old8 = *(void **)(8 * 4);
  _dos_super(ssp);
  _dos_intvcs(2, buserr_h);
  _dos_intvcs(3, addrerr_h);
  _dos_intvcs(8, priv_h);

  /* 1. ユーザーモードとスーパーバイザモードでの読み出し */
  static const struct { unsigned long a; int w; const char *name; } areas[] = {
    {0x000400, 1, "主記憶 $000400 (ベクタ)"},
    {0x002000, 1, "主記憶 $002000"},
    {0xc00000, 1, "グラフィック VRAM $C00000"},
    {0xe00000, 1, "テキスト VRAM $E00000"},
    {0xe82200, 1, "ビデオコントローラ $E82200 (テキストパレット)"},
    {0xe88001, 0, "MFP $E88001 (GPIP)"},
    {0xed0000, 1, "SRAM $ED0000"},
    {0xf00000, 1, "CGROM $F00000"},
    {0xff0000, 1, "IPL ROM $FF0000"},
  };
  /* どれもユーザーモードではバスエラー、スーパーバイザでは読める (XEiJ と Z で確かめた。README の結果) */
  printf("\n[1] 読み出し (ユーザー / スーパーバイザ)\n");
  for (unsigned i = 0; i < sizeof areas / sizeof areas[0]; i++) {
    int u = areas[i].w ? probe_rw(areas[i].a) : probe_rb(areas[i].a);
    unsigned short info = f_info; unsigned long fa = f_addr, pc = f_pc;
    int s = areas[i].w ? sup_rw(areas[i].a) : sup_rb(areas[i].a);
    printf("  %-44s user=%-6s super=%-6s", areas[i].name, kname(u), kname(s));
    if (u == 2)
      printf(" (addr=%06lx info=%04x pc-命令=%ld)", fa, info,
             (long)(pc - (unsigned long)(areas[i].w ? insn_rw : insn_rb)));
    if (u != 2 || s != 0) { printf("  <== 期待と違う (user=BUSERR super=ok のはず)"); ng++; }
    printf("\n");
  }

  /* 2. ユーザーモードからテキスト VRAM へ書く: 止まって、中身が変わらないか ($E7FFFE は表示されない所) */
  printf("\n[2] ユーザーモードからテキスト VRAM $E7FFFE へ書く\n");
  {
    unsigned long a = 0xe7fffe;
    unsigned short before = sup_peekw(a);
    unsigned short pat = before ^ 0x5a5a;
    int r = probe_ww(a, pat);
    unsigned short after = sup_peekw(a);
    expect("書き込み", r, 2);
    if (r) frame(insn_ww);
    printf("    前=%04x 後=%04x %s\n", before, after, before == after ? "(変わっていない)" : "(書けてしまった)");
    if (before != after) { ng++; sup_pokew(a, before); }
  }

  /* 3. 特権命令 */
  printf("\n[3] 特権命令 (ユーザーモード)\n");
  {
    int r = probe_movesr();
    expect("move to SR", r, 8);
    if (r == 8) {
      printf("    PC=%06lx 命令=%06lx %s\n", f_pc, (unsigned long)insn_movesr,
             f_pc == (unsigned long)insn_movesr ? "(違反した命令を指す: やり直せる)" : "(違う所を指す)  <== 期待と違う");
      if (f_pc != (unsigned long)insn_movesr) ng++;
    }
    int v = probe_movefromsr();
    printf("  %-44s %s\n", "move from SR", v & 0x10000 ? kname(v & 0xffff) : "ok (68000 では特権でない)");
    if (v & 0x10000) { printf("    <== 期待と違う (68000 では通るはず)\n"); ng++; }
    else printf("    SR=%04x (S ビット=%d)\n", v & 0xffff, (v >> 13) & 1);
  }

  /* 4. $E86001: 境界と値の関係を実測で決め (XEiJ は (値+1)x8KB、Z は 値x8KB だった)、
   *    8KB 広げてバスエラーになるか見る。さらにこのプログラムの手前まで広げて DOS / IOCS を呼ぶ。最後に元の値へ戻す */
  printf("\n[4] スーパーバイザ領域設定 $E86001 (バイトで書く)\n");
  {
    unsigned long lim = low & ~0x1fffUL;
    if (lim > 0x200000) lim = 0x200000;
    unsigned long b = measure(lim);
    sup_wb(0xe86001, 7);
    unsigned long b7 = measure(lim);
    int off = b7 == 0x10000 ? 1 : b7 == 0xe000 ? 0 : -1;   /* 境界 = (値+off) x 8KB */
    printf("  いまの境界 = %06lx、値 7 を書くと %06lx → 境界 = (値%s) x 8KB\n", b, b7,
           off == 1 ? "+1" : off == 0 ? "" : " ?");
    if (off < 0) { printf("  境界と値の関係が分からない\n"); ng++; off = 0; }
    int orig = b / 0x2000 - off;
    sup_wb(0xe86001, orig);
    expect("元の値に戻して いまの境界 を読む", probe_rw(b), 0);
    set_ipl(7);
    int sr = probe_movefromsr();
    printf("  割り込みを止めた (ユーザーモードの SR=%04x)\n", sr & 0xffff);
    if ((sr & 0x0700) != 0x0700) { printf("  止められなかったので広げる試験はしない\n"); set_ipl(0); ng++; goto skip4; }
    unsigned long nb = b + 0x2000;
    if (nb + 0x2000 > lim) {
      printf("  このプログラムが近すぎて広げられない\n");
    } else {
      sup_wb(0xe86001, nb / 0x2000 - off);
      expect("8KB 広げたあと 旧境界 を読む", probe_rw(b), 2);
      expect("新境界 を読む", probe_rw(nb), 0);
      /* このプログラムの手前 (Human68k、ドライバ、親のシェルを含む) まで守る */
      unsigned long top = lim - 0x2000;
      if (top / 0x2000 - off > 255) top = (255 + off) * 0x2000UL;
      sup_wb(0xe86001, top / 0x2000 - off);
      printf("  境界を %06lx まで広げた\n", top);
      expect("境界の直前 を読む", probe_rw(top - 2), 2);
      int date = _dos_getdate();
      int t = _iocs_ontime().sec;
      printf("  守ったまま DOS _GETDATE=%08x, IOCS _ONTIME=%08x (戻ってきた)\n", date, t);
    }
    /* 4b. 値ごとの境界と、ワードで $E86000 に書いた場合 */
    printf("  -- 書いた値ごとの境界 --\n");
    static const int vals[] = {1, 7, 37, 64, 200, 255};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
      int v = vals[i];
      unsigned long want = (unsigned long)(v + off) * 0x2000;
      if (want >= lim) continue;
      int r = sup_wb(0xe86001, v);
      unsigned long m = measure(lim);
      printf("    byte $E86001 値 %3d: 書き込み=%-6s 境界=%06lx%s\n", v, kname(r), m, m == want ? "" : "  <== 式と違う");
      if (m != want) ng++;
    }
    sup_wb(0xe86001, 1);
    {
      int ssp = _dos_super(0), r = probe_ww(0xe86000, 7);
      _dos_super(ssp);
      unsigned long m = measure(lim);
      printf("    word $E86000 値   7: 書き込み=%-6s 境界=%06lx (%s)\n", kname(r), m,
             m == (unsigned long)(7 + off) * 0x2000 ? "反映された" : "変わらない");
    }
    sup_wb(0xe86001, orig);
    set_ipl(0);
    printf("  元の値 %d に戻した: 境界=%06lx\n", orig, measure(lim));
    if (measure(lim) != b) ng++;
  skip4:;
  }

  /* 5. $EAFF81-$EAFF89: 2MB 以降を 256KB 単位で守る (増設メモリの機能)
   *    今の値を読み出す方法が無い (書き込み専用として扱う) ので、元の値には戻せない。書いたポート ($EAFF81 と、試したブロックの
   *    ポート) は最後に 0 (どこも守らない) になる。ほかのプログラムがこのポートで守っていた範囲は外れる。README にも書いた */
  printf("\n[5] 増設メモリのスーパーバイザ領域設定 $EAFF81-$EAFF89\n");
  {
    int r = sup_wb(0xeaff81, 0);
    printf("  $EAFF81 へ 0 を書く (スーパーバイザ): %s\n", r == 0 ? "ok (ポートがある)" : kname(r));
    if (r == 0) printf("  注意: このポートは今の値を読めないので元に戻せない。書いたポートは 0 (守らない) のまま終わる\n");
    if (r == 0 && ram <= 0x200000) {
      printf("  主記憶が 2MB 以下なので試せない\n");
    } else if (r == 0) {
      /* 2MB より上に 512KB を取り、その中の 256KB 境界のブロックを守ってみる */
      char *p = NULL;
      void *hold[64]; int nh = 0;
      while (nh < 64) {
        char *q = _dos_malloc(0x80000);
        if ((long)q < 0) break;
        if ((unsigned long)q >= 0x200000) { p = q; break; }
        hold[nh++] = q;   /* 2MB より下に取れた分は持っておいて、次を上に追い出す */
      }
      if (!p) {
        printf("  2MB より上に 512KB を取れなかった\n");
      } else {
        unsigned long blk = ((unsigned long)p + 0x3ffff) & ~0x3ffffUL;
        unsigned long port = 0xeaff81 + 2 * ((blk - 0x200000) >> 21);
        int bit = ((blk - 0x200000) >> 18) & 7;
        printf("  ブロック %06lx-%06lx を守る (ポート %06lx bit%d)\n", blk, blk + 0x3ffff, port, bit);
        expect("守る前に ブロック を読む", probe_rw(blk), 0);
        sup_wb(port, 1 << bit);
        expect("守ったあと ブロック を読む", probe_rw(blk), 2);
        expect("守ったあと ブロックの末尾 を読む", probe_rw(blk + 0x3fffe), 2);
        expect("守ったあと すぐ上 (守っていない) を読む", probe_rw(blk + 0x40000 < (unsigned long)p + 0x80000 ? blk + 0x40000 : blk - 2), 0);
        sup_wb(port, 0);   /* 元の値は読めないので 0 (守らない) にする */
        expect("0 に戻したあと ブロック を読む", probe_rw(blk), 0);
        _dos_mfree(p);
      }
      while (nh > 0) _dos_mfree(hold[--nh]);
    }
  }

  /* 6. jsr (An) で守られた所へ飛ぶ。命令の先読みでバスエラーになるとき、戻り番地を USP に積んだ後か前か。
   *    積んだ後: USP = 前 - 4、[USP] = jsr の次の番地。積む前: USP = 前のまま。どちらでも NG にはしない (どちらかでなければ NG)。
   *    Z-OS の prot_ti_fault は、フレームの PC の 2 / 4 バイト前が jsr (An) (0x4E90-0x4E97) ならそこから戻り番地を決め、
   *    [USP] がそれと同じときだけ積んだとみなす。その判定が実機で合うかもここで見る */
  printf("\n[6] ユーザーモードから守られた所へ jsr (a1)\n");
  {
    static const struct { unsigned long a; const char *name; } tg[] = {
      {0x002000, "主記憶 $002000 ($E86001 で守られた所)"},
      {0xff0000, "IPL ROM $FF0000"},
    };
    for (unsigned i = 0; i < sizeof tg / sizeof tg[0]; i++) {
      unsigned long ret = (unsigned long)insn_jsr + 2;
      printf("  %s\n", tg[i].name);
      if (probe_rw(tg[i].a) != 2) { printf("    ユーザーモードで読めてしまうので飛ばない\n"); ng++; continue; }
      int r = probe_jsr(tg[i].a);
      expect("jsr (a1)", r, 2);
      if (r != 2) continue;
      unsigned long fpc = ((unsigned long)f_frame[5] << 16) | f_frame[6];
      unsigned long fa = ((unsigned long)f_frame[1] << 16) | f_frame[2];
      printf("    frame: info=%04x addr=%06lx ir=%04x sr=%04x pc=%06lx\n", f_frame[0], fa, f_frame[3], f_frame[4], fpc);
      printf("    jsr の番地=%06lx 次の番地=%06lx pc-jsr=%ld pc-飛び先=%ld\n", (unsigned long)insn_jsr, ret,
             (long)(fpc - (unsigned long)insn_jsr), (long)(fpc - tg[i].a));
      printf("    USP 前=%06lx 後=%06lx [USP]=%08lx\n", f_usp0, f_usp, f_usptop);
      int pushed = f_usp == f_usp0 - 4 && f_usptop == ret;
      int notpushed = f_usp == f_usp0;
      printf("    → %s\n", pushed ? "戻り番地を積んでからバスエラー" : notpushed ? "戻り番地を積む前にバスエラー" : "どちらでもない  <== 期待と違う");
      if (!pushed && !notpushed) ng++;
      /* prot_ti_fault の判定をまねる */
      unsigned long z = 0;
      if (!(fpc & 1) && fpc >= 4) {
        int ssp = _dos_super(0);
        unsigned short w2 = *(volatile unsigned short *)(fpc - 2), w4 = *(volatile unsigned short *)(fpc - 4);
        _dos_super(ssp);
        if ((w2 & 0xfff8) == 0x4e90) z = fpc; else if ((w4 & 0xfff8) == 0x4e90) z = fpc - 2;
      }
      int zpushed = z ? f_usptop == z : 1;
      unsigned long zret = z && !zpushed ? z : f_usptop;
      printf("    Z-OS の判定: %s、戻り番地=%06lx %s\n", zpushed ? "積んだ" : "積んでいない", zret,
             zret == ret && zpushed == pushed ? "(合っている)" : "  <== 合わない");
      if (zret != ret || zpushed != pushed) ng++;
    }
  }

  _dos_intvcs(2, old2);
  _dos_intvcs(3, old3);
  _dos_intvcs(8, old8);
  printf("\n%s\n", ng ? "NG: 期待と違う所がある" : "OK");
  return ng ? 1 : 0;
}
