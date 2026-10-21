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
  else if (cp < 0x10000) { fputc(0xe0 | (cp >> 12), f); fputc(0x80 | ((cp >> 6) & 0x3f), f); fputc(0x80 | (cp & 0x3f), f); }
  else { fputc(0xf0 | (cp >> 18), f); fputc(0x80 | ((cp >> 12) & 0x3f), f);
         fputc(0x80 | ((cp >> 6) & 0x3f), f); fputc(0x80 | (cp & 0x3f), f); }
}

/* 画面の文字を COLS x ROWS で書き出す。全角の継続セル (F_WIDECONT) は出さない
 * (pyte の display と合わせる: 全角は 1 文字 + 空の右セルで、文字列長はセル数より短くなる)。 */
void vt_dump_text(FILE *f, int cols, int rows)
{
  int r, c;
  for (r = 0; r < rows; r++) {
    for (c = 0; c < cols; c++) {
      unsigned short cpv; unsigned char fg, bg, fl;
      long cp;
      vt_get_cell(r, c, &cpv, &fg, &bg, &fl);
      if (fl & 8) continue;                                /* F_WIDECONT: 右半分は出さない */
      if (fl & 16) cp = (long)vt_emoji_cp(cpv);            /* F_EMOJI: インデックス -> コードポイント */
      else if (fl & 2) cp = special_to_cp((unsigned char)cpv);  /* F_SPECIAL */
      else cp = cpv;
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
      unsigned short cpv; unsigned char fg, bg, fl;
      vt_get_cell(r, c, &cpv, &fg, &bg, &fl);
      fprintf(f, "%x,%x,%x ", fg, bg, fl);
    }
    fputc('\n', f);
  }
}
