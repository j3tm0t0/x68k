/*
 * vtdump.c -- 論理画面を UTF-8 テキストで書き出す (pyte の screen.display と比べるため)。
 * vtcat.x (X68000) と vthost (Mac テスト) の両方から使う。
 */
#include <stdio.h>
#include "vt100.h"

/* DEC 特殊グリフ (F_SPECIAL) のバイト -> Unicode コードポイント (pyte の変換と合わせる) */
static long special_to_cp(unsigned char ch)
{
  switch (ch) {
    case 0x71: return 0x2500;  /* ─ */
    case 0x78: return 0x2502;  /* │ */
    case 0x6c: return 0x250c;  /* ┌ */
    case 0x6b: return 0x2510;  /* ┐ */
    case 0x6d: return 0x2514;  /* └ */
    case 0x6a: return 0x2518;  /* ┘ */
    case 0x74: return 0x251c;  /* ├ */
    case 0x75: return 0x2524;  /* ┤ */
    case 0x77: return 0x252c;  /* ┬ */
    case 0x76: return 0x2534;  /* ┴ */
    case 0x6e: return 0x253c;  /* ┼ */
    case 0x61: return 0x2592;  /* ▒ */
    case 0x60: return 0x25c6;  /* ◆ */
    case 0x6f: return 0x23ba;  /* ⎺ */
    case 0x73: return 0x23bd;  /* ⎽ */
    default:   return ch;
  }
}

static void put_utf8(FILE *f, long cp)
{
  if (cp < 0x80) fputc((int)cp, f);
  else if (cp < 0x800) { fputc(0xc0 | (cp >> 6), f); fputc(0x80 | (cp & 0x3f), f); }
  else { fputc(0xe0 | (cp >> 12), f); fputc(0x80 | ((cp >> 6) & 0x3f), f); fputc(0x80 | (cp & 0x3f), f); }
}

/* 画面の文字だけを COLS x ROWS で書き出す */
void vt_dump_text(FILE *f, int cols, int rows)
{
  int r, c;
  for (r = 0; r < rows; r++) {
    for (c = 0; c < cols; c++) {
      unsigned char cell[4];
      long cp;
      vt_get_cell(r, c, cell);
      if (cell[3] & 2) cp = special_to_cp(cell[0]);  /* F_SPECIAL */
      else cp = cell[0];
      if (cp < 0x20) cp = ' ';
      put_utf8(f, cp);
    }
    fputc('\n', f);
  }
}

/* 属性 (fg,bg,flags) を 1 セル 3 バイトの 16 進で書き出す (属性比較用) */
void vt_dump_attr(FILE *f, int cols, int rows)
{
  int r, c;
  for (r = 0; r < rows; r++) {
    for (c = 0; c < cols; c++) {
      unsigned char cell[4];
      vt_get_cell(r, c, cell);
      fprintf(f, "%x,%x,%x ", cell[1], cell[2], cell[3]);
    }
    fputc('\n', f);
  }
}
