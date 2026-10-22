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
#define OP_ECHOACK 0x0f

static int zd_bye = -1, zd_mode;

void ztd_init(int cols, int rows) { vt_init(cols, rows); }
int ztd_bye_code(void) { return zd_bye; }
int ztd_mode(void) { return zd_mode; }
void ztd_cursor(int *x, int *y, int *vis) { *x = cx; *y = cy; *vis = cur_vis; }


/* ================= 予測エコー (mosh と同じ考え) =================
 * 打ったキー (印字 ASCII・BS・←→) の結果をサーバの返事を待たずに描き、下線で「未確認」と示す。
 * 予測はサーバの画面 (cur) の上に重ねるだけ: フレームを置く前に元のセルに戻し、置いた後で照合して、残るものだけ重ね直す
 * (サーバの差分は Z の論理画面がサーバの控えと同じことを前提にしているので、予測を論理画面に残さない)。
 * 照合: サーバの ECHOACK (キーの先頭 n バイトは pty に書いてから 100ms 以上経った画面) と、予測ごとのキーの通し番号で
 *   - そのセルに予測した字が出た → 確定 (重ねるのをやめる。その場面 (下の ctx) を「信用」に)
 *   - ack が予測のキーを越えたのに出ていない → 外れ: 全部取り消し、その場面を「不信」に
 *   - 1.5 秒経っても決まらない → 外れと同じ
 * 信用していない場面でも予測は作るが描かない (影の予測)。影の予測が当たれば信用に変わる。パスワード入力・エコーしない
 * 全画面アプリでは当たらないので描かれない。場面 (ctx) は「カーソル非表示か」「代替画面か」の 4 通りで別々に信用を持つ。
 * Enter・Tab・Ctrl・他のエスケープの後は、それまでの予測が決まるまで新しい予測をしない。 */
#define PR_MAX 64
typedef struct { unsigned char r, c, shown, same; unsigned short cp; cell_t base; unsigned long seq; long t; unsigned epoch; } pred_t;
static pred_t pr[PR_MAX];
static int pr_n;                 /* 未確定の予測の数 */
static int pr_on;                /* サーバが ECHOACK を送ってくる (予測してよい) */
static int pr_want = 1;          /* 利用者の設定 (-oZtermEcho=no で 0) */
static int pr_trust[4];          /* 場面ごとの信用 */
static int pr_hold;              /* 予測できないキーの後: 決まるまで予測しない */
static int pr_cx, pr_cy, pr_curset;   /* 予測したカーソル位置 (pr_curset のとき) */
static int srv_cx, srv_cy, srv_vis = 1;
static unsigned long pr_ack, pr_lastseq;
static long pr_nhit, pr_nmiss;
static unsigned pr_epoch;        /* Enter などで進む。前の epoch の予測が当たっても信用にしない */
#define PR_TIMEOUT 1500

#if defined(VT_SIM_TXRAS)
static long pr_ms(void) { return 0; }   /* 68000 シミュレータ (libc 無し) */
#elif defined(VT_HOSTTEST)
#include <sys/time.h>
static long pr_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000L + tv.tv_usec / 1000; }
#else
static long pr_ms(void) { return (long)_iocs_ontime().sec * 10; }   /* 1/100 秒 (日付が変わると戻るが、外れ扱いになるだけ) */
#endif
#if defined(VT_HOSTTEST) && !defined(VT_SIM_TXRAS)
#include <stdio.h>
#include <stdlib.h>
#define PRDBG(...) do { if (getenv("ZTPR")) { fprintf(stderr, "[pr %ld] ", pr_ms() % 100000); fprintf(stderr, __VA_ARGS__); } } while (0)
#else
#define PRDBG(...) do { } while (0)
#endif
/* 行 r の予測込みで最後の空白でない桁 (無ければ -1) */
static unsigned short pr_cp_at(int r, int c);
static int pr_last(int r)
{
  int c;
  for (c = COLS - 1; c >= 0; c--) if (pr_cp_at(r, c) != 0x20) return c;
  return -1;
}
/* 桁 c0..c1 に全角・絵文字・DEC 罫線が無いか (ずらす予測は半角の字だけ) */
static int pr_plain(int r, int c0, int c1)
{
  int c;
  for (c = c0; c <= c1 + 1 && c < COLS; c++) if (cur[r][c].at & (F_WIDE | F_WIDECONT | F_EMOJI | F_SPECIAL)) return 0;
  return 1;
}
/* (r,c) の予測込みの字 (後の予測が優先) */
static unsigned short pr_cp_at(int r, int c)
{
  int i;
  for (i = pr_n - 1; i >= 0; i--) if (pr[i].r == r && pr[i].c == c) return pr[i].cp;
  return cur[r][c].cp;
}
static int pr_ctx(void) { return (srv_vis ? 0 : 1) | ((zd_mode & 2) ? 2 : 0); }

void ztd_echo(int on) { pr_want = on; }
void ztd_echo_stats(long *hit, long *miss) { *hit = pr_nhit; *miss = pr_nmiss; }
int ztd_echo_shown(void) { int i, k = 0; for (i = 0; i < pr_n; i++) k += pr[i].shown; return k; }   /* いま描いている予測の数 */

/* 予測を重ねる / 外す (論理画面のセルと書き換え範囲だけ。描くのは flush) */
static void pr_show(void)
{
  int i, t = pr_trust[pr_ctx()];
  for (i = 0; i < pr_n; i++) {
    pred_t *q = &pr[i];
    q->shown = (unsigned char)t;
    if (!t) continue;
    cur[q->r][q->c].cp = q->cp;
    cur[q->r][q->c].at = (unsigned short)((q->base.at & 0xff00) | F_UNDERLINE);   /* 色はそのセルのまま、下線で未確認 */
    if (q->cp == 0x20) cur[q->r][q->c].at &= (unsigned short)~F_UNDERLINE;      /* BS の空白は下線にしない */
    touch_span(q->r, q->c, q->c);
  }
  if (t && pr_curset) { cx = pr_cx; cy = pr_cy; cur_vis = 1; }
  else { cx = srv_cx; cy = srv_cy; cur_vis = srv_vis; }
}
static void pr_hide(void)
{
  int i;
  for (i = pr_n - 1; i >= 0; i--)   /* 後ろから (同じセルに重ねた予測があっても最初の元に戻る) */
    if (pr[i].shown) { cur[pr[i].r][pr[i].c] = pr[i].base; touch_span(pr[i].r, pr[i].c, pr[i].c); pr[i].shown = 0; }
  cx = srv_cx; cy = srv_cy; cur_vis = srv_vis;
}

/* フレームを置いた後: 照合して残りを重ね直す (予測は pr_hide で外してある = cur はサーバの画面) */
static void pr_reconcile(int reset)
{
  int i, j, miss = 0, ctx = pr_ctx();
  long now = pr_ms();
  if (reset) { pr_n = 0; pr_curset = 0; pr_hold = 0; }
  for (i = j = 0; i < pr_n; i++) {
    pred_t *q = &pr[i];
    cell_t *s = &cur[q->r][q->c];
    q->base = *s;
    if (s->cp == q->cp && !(s->at & (F_WIDECONT | F_WIDE | F_EMOJI))) {   /* 当たり */
      if (!q->same && q->epoch == pr_epoch) pr_trust[ctx] = 1;
      pr_nhit++;
      continue;
    }
    if ((long)(pr_ack - q->seq) >= 0 || now - q->t > PR_TIMEOUT || now < q->t) {   /* 外れ */
      PRDBG("miss (%d,%d) want %04x got %04x ack %lu seq %lu age %ld\n", q->r, q->c, q->cp, s->cp, pr_ack, q->seq, now - q->t);
      miss = 1; break;
    }
    pr[j++] = *q;
  }
  if (miss) {
    pr_n = 0; pr_curset = 0; pr_hold = 0;
    pr_trust[ctx] = 0; pr_nmiss++;
  } else {
    pr_n = j;
    if ((long)(pr_ack - pr_lastseq) >= 0) {
      /* キーが全部届いて画面に反映された: 残った予測は無いはず。カーソルの予測も照合する */
      if (pr_curset && (pr_cx != srv_cx || pr_cy != srv_cy)) { PRDBG("cursor miss pred %d,%d srv %d,%d\n", pr_cx, pr_cy, srv_cx, srv_cy); pr_trust[ctx] = 0; pr_nmiss++; }
      pr_n = 0; pr_curset = 0; pr_hold = 0;
    }
  }
  pr_show();
}

/* 予測を 1 つ足す (r,c に cp)。base はサーバのセル */
static int pr_add(int r, int c, unsigned short cp, unsigned long seq)
{
  cell_t *s;
  int i;
  if (r >= ROWS || c >= COLS) return 0;
  for (i = 0; i < pr_n; i++)   /* 同じセルの予測は上書き (BS の空白の上に打ち直した字など) */
    if (pr[i].r == r && pr[i].c == c) { pr[i].cp = cp; pr[i].seq = seq; pr[i].t = pr_ms(); pr[i].same = (unsigned char)(pr[i].base.cp == cp); pr[i].epoch = pr_epoch; return 1; }
  if (pr_n >= PR_MAX) return 0;
  s = &cur[r][c];
  if (s->at & (F_WIDE | F_WIDECONT | F_EMOJI)) return 0;   /* 全角の上には予測しない */
  pr[pr_n].r = (unsigned char)r; pr[pr_n].c = (unsigned char)c; pr[pr_n].cp = cp;
  pr[pr_n].base = *s; pr[pr_n].seq = seq; pr[pr_n].t = pr_ms(); pr[pr_n].shown = 0; pr[pr_n].epoch = pr_epoch;
  pr[pr_n].same = (unsigned char)(s->cp == cp);   /* 元から同じ字: 当たっても信用の証拠にしない */
  pr_n++;
  return 1;
}

/* キーを送る前に呼ぶ (zc_keys)。seq はこのキー列を送った後のキーの通し番号 */
void ztd_predict(const unsigned char *k, int n, unsigned long seq)
{
  int i, sup, x, y;
  if (!pr_on || !pr_want || !rows_main[0]) return;
  pr_lastseq = seq;
  sup = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }
  pr_hide();
  x = pr_curset ? pr_cx : srv_cx;
  y = pr_curset ? pr_cy : srv_cy;
  for (i = 0; i < n; i++) {
    unsigned char b = k[i];
    if (pr_hold) break;
    if (b >= 0x20 && b < 0x7f) {
      int e = pr_last(y), c;
      if (x >= COLS - 1) { pr_hold = 1; break; }   /* 右端 (折り返し) は予測しない */
      if (e >= x) {   /* 行の途中: 右の字を 1 つずつずらす (挿入。readline も Claude Code の入力欄も挿入) */
        if (e + 1 >= COLS || !pr_plain(y, x, e)) { pr_hold = 1; break; }
        for (c = e; c >= x; c--) pr_add(y, c + 1, pr_cp_at(y, c), seq);
      }
      if (!pr_add(y, x, b, seq)) { pr_hold = 1; break; }
      x++;
    } else if (b == 0x7f) {
      if (pr_n > 0 && pr[pr_n - 1].r == y && pr[pr_n - 1].c == x - 1 && pr[pr_n - 1].cp != 0x20 && pr_last(y) < x) {   /* 行末の自分の予測を消す */
        pred_t *q = &pr[pr_n - 1];
        if (q->base.cp == 0x20) pr_n--;                       /* 元が空白なら予測ごと無くす */
        else { q->cp = 0x20; q->seq = seq; q->t = pr_ms(); }  /* 元に字があった (BS の後に打ち直した) なら空白の予測に戻す */
        x--;
      }
      else {
        int e = pr_last(y), c;
        if (x <= 0 || x - 1 > e + 1) { pr_hold = 1; break; }
        if (e >= x) {   /* 行の途中: 右の字を詰める */
          if (!pr_plain(y, x - 1, e)) { pr_hold = 1; break; }
          for (c = x - 1; c < e; c++) pr_add(y, c, pr_cp_at(y, c + 1), seq);
          pr_add(y, e, 0x20, seq);
        } else if (!pr_add(y, x - 1, 0x20, seq)) { pr_hold = 1; break; }
        x--;
      }
    } else if (b == 0x1b && i + 2 < n && (k[i + 1] == '[' || k[i + 1] == 'O') && (k[i + 2] == 'C' || k[i + 2] == 'D')) {
      if (k[i + 2] == 'D') { if (x <= 0) { pr_hold = 1; break; } x--; }
      else { if (x >= COLS - 1 || pr_cp_at(y, x) == 0x20) { pr_hold = 1; break; } x++; }   /* 文字の無い所へは進めない */
      i += 2;
    } else {   /* Enter・Tab・Ctrl・他のエスケープ: 予測をやめ、信用も白紙に (次の行・パスワード入力・アプリの切り替わりかもしれない。
                  mosh の epoch と同じ: 新しい行の最初の字は影の予測で、当たるまで描かない) */
      int t;
      for (t = 0; t < 4; t++) pr_trust[t] = 0;
      pr_epoch++;
      pr_hold = 1;
      break;
    }
  }
  pr_cx = x; pr_cy = y; pr_curset = !pr_hold;   /* 予測できないキーの後のカーソルは分からない: サーバのを出す */
  PRDBG("keys %d seq %lu -> pending %d cursor %d,%d hold %d trust %d\n", n, seq, pr_n, x, y, pr_hold, pr_trust[pr_ctx()]);
  pr_show();
  flush();
  sup_leave(sup);
}

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
  int reset = 0;
  sup = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }   /* scroll が VRAM を動かす前にカーソルを消す */
  if (rows_main[0]) pr_hide();   /* 予測を外してサーバの画面に戻してから置く */
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
        reset = 1;   /* 行が動いた: 予測の位置は意味を失う */
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
        cx = srv_cx = p[0] < COLS ? p[0] : COLS - 1;
        cy = srv_cy = p[1] < ROWS ? p[1] : ROWS - 1;
        cur_vis = srv_vis = p[2] & 1;
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
        reset = 1;
        for (rr = 0; rr < ROWS; rr++) { for (cc = 0; cc < COLS; cc++) cur[rr][cc] = z; TOUCH(rr); }
        break;
      }
      case OP_HELLO:
        NEED(3);
        if (p[0] != 1) { ret = -1; goto out; }
        sup_leave(sup);
        vt_init(p[1], p[2]);
        sup = sup_enter();
        reset = 1; srv_cx = srv_cy = 0; srv_vis = 1;
        p += 3; r = 0; c = 0; row = cur[0];
        break;
      case OP_SIZE:
        NEED(2); COMMIT();
        sup_leave(sup);
        vt_init(p[0], p[1]);
        sup = sup_enter();
        reset = 1; srv_cx = srv_cy = 0; srv_vis = 1;
        p += 2; r = 0; c = 0; row = cur[0];
        break;
      case OP_ECHOACK:
        NEED(4);
        pr_ack = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
        pr_on = 1;
        p += 4;
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
  if (rows_main[0]) pr_reconcile(reset);
  flush();
  sup_leave(sup);
  return ret;
#undef COMMIT
#undef NEED
}
