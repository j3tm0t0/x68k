/*
 * ztdraw.c -- zterm のフレームを term/vt100 の論理画面に置いて描く。
 *
 * vt100.c の内部 (論理画面 cur・書き換え範囲 rlo/rhi・scroll・flush・カーソル) をそのまま使うため、
 * vt100.c をここに #include する (vt100.c 自体は変えない。ssh.x の ZTERM=1 ビルドと zthost は vt100.c の
 * 代わりにこのファイルをコンパイルする)。VT の解析 (vt_write) は通らない: サーバが Z の描ける形の
 * セル (cp + 色 + フラグ) まで決めてくるので、ここではセルを書いて flush するだけ。
 */
#include "vt100.c"
#include "ztclient.h"

/* フレーム命令 (term/zterm/server/src/proto.rs と同じ) */
#define OP_HELLO  0x01
#define OP_POS    0x02
#define OP_ATTR   0x03
#define OP_WIDE   0x04
#define OP_EMOJI  0x05
#define OP_CP16   0x06
#define OP_REP    0x07
#define OP_SCROLL 0x08
#define OP_CURSOR 0x09
#define OP_MODE   0x0a
#define OP_BELL   0x0b
#define OP_CLEAR  0x0c
#define OP_BYE    0x0d
#define OP_SIZE   0x0e

static int zd_bye = -1, zd_mode;

void ztd_init(int cols, int rows) { vt_init(cols, rows); }
int ztd_bye_code(void) { return zd_bye; }
int ztd_mode(void) { return zd_mode; }

/* 1 フレームの本体 p[0..n) を適用する。書き込み位置は (0,0)、属性は (7,0,0) から。
 * 行ごとに書いた桁の範囲を lo..hi に溜め、行が変わるときに rlo/rhi に足す (セルごとに touch_span を呼ばない)。 */
int ztd_frame(const unsigned char *p, int n)
{
  const unsigned char *e = p + n;
  int r = 0, c = 0, lo = MAXCOLS, hi = -1, sup, ret = 0;
  unsigned short at = AT(7, 0, 0);
  cell_t *row = cur[0];
#define COMMIT() do { if (hi >= lo && r < ROWS) { if (lo < rlo[r]) rlo[r] = (unsigned char)lo; if (hi > rhi[r]) rhi[r] = (unsigned char)hi; } lo = MAXCOLS; hi = -1; } while (0)
#define NEED(k) do { if (e - p < (k)) { ret = -1; goto out; } } while (0)
  sup = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }   /* scroll が VRAM を動かす前にカーソルを消す */
  while (p < e) {
    unsigned char b = *p++;
    if (b >= 0x20 && b < 0x7f) {   /* ASCII: 連なりをまとめて置く */
      if (r < ROWS) {
        if (c < lo) lo = c;
        for (;;) {
          if (c < COLS) { row[c].cp = b; row[c].at = at; }
          c++;
          if (p >= e || *p < 0x20 || *p >= 0x7f) break;
          b = *p++;
        }
        { int h = (c <= COLS ? c : COLS) - 1; if (h > hi) hi = h; }
      } else {
        while (p < e && *p >= 0x20 && *p < 0x7f) p++;
      }
      continue;
    }
    switch (b) {
      case OP_POS:
        NEED(2); COMMIT();
        r = p[0]; c = p[1]; p += 2;
        if (r < ROWS) row = cur[r];
        break;
      case OP_ATTR:
        NEED(2);
        at = AT(p[0] >> 4, p[0] & 15, p[1] & (F_UNDERLINE | F_SPECIAL)); p += 2;
        break;
      case OP_WIDE: case OP_EMOJI: {
        unsigned short cp;
        unsigned short f = (unsigned short)(b == OP_EMOJI ? (F_WIDE | F_EMOJI) : F_WIDE);
        if (b == OP_EMOJI) { NEED(1); cp = p[0]; p += 1; if (cp >= EMOJITAB_N) cp = 0; }
        else { NEED(2); cp = (unsigned short)((p[0] << 8) | p[1]); p += 2; }
        if (r < ROWS && c + 1 < COLS) {
          row[c].cp = cp; row[c].at = (unsigned short)((at & ~F_SPECIAL) | f);
          row[c + 1].cp = 0x20; row[c + 1].at = (unsigned short)((at & ~F_SPECIAL) | F_WIDECONT);
          if (c < lo) lo = c;
          if (c + 1 > hi) hi = c + 1;
        }
        c += 2;
        break;
      }
      case OP_CP16:
        NEED(2);
        if (r < ROWS && c < COLS) {
          row[c].cp = (unsigned short)((p[0] << 8) | p[1]); row[c].at = at;
          if (c < lo) lo = c;
          if (c > hi) hi = c;
        }
        p += 2; c++;
        break;
      case OP_REP: {
        int k, m;
        NEED(2);
        k = p[0]; m = p[1]; p += 2;
        if (r < ROWS && k > 0) {
          int c1 = c + k - 1, i;
          if (c1 >= COLS) c1 = COLS - 1;
          for (i = c; i <= c1; i++) { row[i].cp = (unsigned short)m; row[i].at = at; }
          if (c < lo) lo = c;
          if (c1 > hi) hi = c1;
        }
        c += k;
        break;
      }
      case OP_SCROLL: {
        int t, bt, k;
        NEED(3); COMMIT();
        t = p[0]; bt = p[1]; k = (signed char)p[2]; p += 3;
        if (t < ROWS && bt < ROWS && t < bt) {
          top = t; bot = bt;
          scroll(k);
          top = 0; bot = ROWS - 1;
        }
        if (r < ROWS) row = cur[r];   /* 行ポインタが回った */
        break;
      }
      case OP_CURSOR:
        NEED(3);
        cx = p[0] < COLS ? p[0] : COLS - 1;
        cy = p[1] < ROWS ? p[1] : ROWS - 1;
        cur_vis = p[2] & 1;
        p += 3;
        break;
      case OP_MODE:
        NEED(1); zd_mode = p[0]; p += 1;
        break;
      case OP_BELL:
        break;
      case OP_CLEAR: {
        int rr, cc;
        cell_t z;
        NEED(1); COMMIT();
        z.cp = 0x20; z.at = AT(7, p[0] & 15, 0); p += 1;
        for (rr = 0; rr < ROWS; rr++) { for (cc = 0; cc < COLS; cc++) cur[rr][cc] = z; TOUCH(rr); }
        break;
      }
      case OP_HELLO:
        NEED(3);
        if (p[0] != 1) { ret = -1; goto out; }
        sup_leave(sup);
        vt_init(p[1], p[2]);
        sup = sup_enter();
        p += 3; r = 0; c = 0; row = cur[0];
        break;
      case OP_SIZE:
        NEED(2); COMMIT();
        sup_leave(sup);
        vt_init(p[0], p[1]);
        sup = sup_enter();
        p += 2; r = 0; c = 0; row = cur[0];
        break;
      case OP_BYE:
        NEED(1); zd_bye = p[0]; p += 1; ret = 1;
        break;
      default:
        ret = -1; goto out;
    }
  }
out:
  COMMIT();
  flush();
  sup_leave(sup);
  return ret;
#undef COMMIT
#undef NEED
}
