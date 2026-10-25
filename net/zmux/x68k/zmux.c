/*
 * zmux.sys -- RS-232C 1 本でチャネルを多重化する線の X68000 側 (net/zmux/DESIGN.md)
 *
 *   CONFIG.SYS:   DEVICE = \SYS\ZMUX.SYS       (RSDRV.SYS の後ろ)
 *   AUTOEXEC.BAT: ZMUX                         (zmux.x: 線を回すスレッドを作る)
 *                 CTTY AUX                     (コンソールをチャネル 0 に)
 *
 * 文字デバイスの名前は "AUX"。後から組み込んだデバイスが先に見つかるので、RSDRV.SYS の AUX の代わりになる
 * (COMMAND.X 3.00 の CTTY は con と aux しか受け付けないので、別の名前にはできない)。AUX0〜AUX5 は RSDRV.SYS のまま。
 *
 * 線の読み書きは IOCS (_ISNS232C / _INP232C / _OSNS232C / _OUT232C、つまり ROM と RSDRV.SYS) でポーリングする。
 * 自前の SCC 割り込みは使わない (slip.sys で xip.x が壊れた前例)。RSDRV.SYS の受信バッファは 32KB に付け替える。
 * 線を回す処理 (zl_poll) は、zmux.x のスレッドと AUX の読み書きの両方から呼ぶ。スレッドは UP の間だけ回す
 * (素の文字の間は rx.x などが IOCS で直接読み書きしているかもしれないので、横取りしない)。どちらもスーパーバイザで動き、
 * Human68k はスーパーバイザの間はスレッドを切り替えないので、同時には走らない。
 *
 * 状態は 2 つ:
 *   RAW: 素の文字。AUX の出力はそのまま線へ、線の文字はそのまま AUX の入力へ (RSDRV.SYS の AUX と同じ)。
 *        ただし DLE STX ... DLE ETX のフレームは拾って調べる (Pi の HELLO_ACK を待つ)。AUX を読み書きしている間、
 *        5 秒ごとに HELLO を送る。
 *   UP:  フレームだけ。AUX はチャネル 0。Pi から 10 秒フレームが来なければ RAW に戻る。
 *
 * フレーム: DLE STX | chan seq ack flags len(2) | data | crc16(2) | DLE ETX (中の DLE は DLE DLE)
 * 送り直しは Go-Back-N (窓 4)。
 */
#define __IOCS_INLINE__
#include <x68k/iocs.h>
#include <stdint.h>

#define DLE 0x10
#define STX 0x02
#define ETX 0x03

#define MAXLEN   1024
#define WIN      4
#define RINGSZ   2048           /* チャネル 0 のリング (2 の累乗) */
#define OUTSZ    8192           /* 線に出すバイトの待ち行列 (2 の累乗) */
#define RXBUF    32768          /* RSDRV.SYS の受信バッファの付け替え先 */

#define F_NAK    0x01
#define F_BUSY   0x02
#define F_UNSEQ  0x04           /* 番号を使わないフレーム (HELLO / HELLO_ACK / KICK) */
#define F_END    0x08           /* チャネル 3: 交換のブロックの最後のフレーム */

#define CH_CON   0
#define CH_NP    3              /* netpipe の交換 (npd.x -m) */
#define CH_RD    2              /* リモートドライブ (zrmt.sys の要求と応答、1 つずつ) */
#define CH_KEY   1              /* 入力の注入 (Pi → X68000)。メッセージ = 種類 1 バイト + 中身 (下の IN_*) */
#define IN_KEY   1              /* キー: 2 バイト (スキャンコード, 文字コード) = _B_KEYINP の値の上位・下位 */
#define IN_MOUSE 2              /* マウス (予定、まだ処理しない): dx(int8) dy(int8) buttons(bit0 左 bit1 右) の 3 バイト */
#define CH_CTL   15
#define OP_HELLO     1
#define OP_HELLO_ACK 2
#define OP_BYE       3
#define OP_KICK      4          /* Pi に X68000 へ渡すものがある (npd.x に交換を促す) */
#define BLK      4112           /* 交換のブロックの最大 (見出し 16 + 8 セクタ - 16) */

#define T_RESEND 100            /* 1/100 秒 */
#define T_ACK    5
#define T_HELLO  500
#define T_DEAD   6000           /* 60 秒: フロッピーから大きな .x を読む間に切れないように */
#define T_KEEP   200            /* 送るものが無くても 2 秒に 1 回は確認だけのフレームを出す (Pi が 10 秒で切らないように) */

#define VERSION  1

struct req_header {
  uint8_t magic, unit, command, errl, errh, reserved[8], attr;
  uint8_t *addr;
  uint32_t status;
  void *fcb;
} __attribute__((packed, aligned(2)));

extern void dos_print(const char *);

struct ring { uint16_t head, tail; uint8_t b[RINGSZ]; };
static struct ring rxr, txr;    /* rxr: Pi → AUX の入力、txr: AUX の出力 → Pi */
static uint8_t outq[OUTSZ];
static uint16_t oh, ot;

static int up;                  /* 0 = RAW, 1 = UP */
static uint8_t tx_seq;          /* 次に送るデータのフレームの番号 */
static uint8_t tx_base;         /* 確認の来ていない最初の番号 */
static uint8_t rx_expect;       /* 次に受けるはずの番号 */
static int need_ack, nak_pending;
static long t_sent, t_ack_due, t_last_rx, t_hello, t_last_tx;
static int started;             /* zl_poll が一度でも呼ばれた */

/* 確認の来ていないフレームの写し (送り直し用) */
static struct { uint16_t len; uint8_t chan, flags; uint8_t d[MAXLEN]; } win[WIN];

/* チャネル 3: npd.x の交換。要求のブロックを分けて送り、応答のブロックを組み立てる (同時に 1 つ) */
static uint8_t btx[BLK], brx[BLK];
static int btx_len, btx_pos;            /* btx_pos < btx_len の間は送りかけ */
static int brx_len, brx_done;
static int kick;

/* チャネル 2: zrmt.sys の要求 1 つを送り、応答 1 つを zrmt.sys の場所 (rd_buf) に組み立てる (同時に 1 つ) */
static uint8_t rtx[BLK];
static int rtx_len, rtx_pos;
static uint8_t *rd_buf;
static int rd_max, rd_len, rd_done;

/* 受けているフレーム */
static uint8_t fr[8 + MAXLEN];
static int fpos, pstate;        /* pstate: 0 = 外, 1 = 外で DLE, 2 = 中, 3 = 中で DLE */

static uint16_t crctab[256];

/* 注入のキー (_B_KEYINP の戻り値と同じ形) */
#define KEYQ 64
static uint16_t keyq[KEYQ];
static uint8_t in_need, in_n, in_buf[4];   /* チャネル 1 のメッセージの途中経過 */
static volatile uint8_t kh, kt;
extern void *old_keysns, *old_keyinp;
extern void hk_keysns(void), hk_keyinp(void);
extern long call_orig(void *entry, long num);
static void zl_poll(void);

static void zl_poll(void);
long key_peek(void)
{
  /* 画面が CON のときの COMMAND.X は、キーを待つあいだ _B_KEYSNS だけを繰り返す (_B_KEYINP は押されてから)。ここで線を回さないと、
     常駐スレッドは線が UP になるまで何もしないので、zmuxd をつないでも UP にならない */
  if (!up) {
    /* 線が UP になった後は常駐スレッドが回す。割り込みの中 (IPL が 0 でない) から呼ばれたときは回さない: AUX の書き込み (con_out) と読み出しは
       lock なしで txr / rxr を触るので、そこへ割り込みから zl_poll が入ると送るバイトが壊れる (実機 XVI で ver が化けた) */
    uint16_t sr;
    __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
    if (!(sr & 0x0700))
      zl_poll();
  }
  return kh != kt ? keyq[kt] : 0;
}

/* _B_KEYINP: 注入のキーがあれば返す。無ければ本物のキーが押されるか注入が来るまで待つ (待つ間も線を回す) */
long key_get(void)
{
  for (;;) {
    if (kh != kt) {
      long c = keyq[kt];
      kt = (kt + 1) % KEYQ;
      return c;
    }
    if (call_orig(old_keysns, 1))
      return call_orig(old_keyinp, 0);
    zl_poll();
  }
}
static uint8_t rxbuf[RXBUF];

/* 動作確認用 (メモリダンプで "ZMUXSTAT" の後ろ) */
static struct { char tag[8]; uint32_t rxf, txf, bad, resend, rawin, rawout, ups, downs, polls, maxgap, lastpoll, gapat, outbytes, outq, refused, win, lockfail, downat, lastrx_at_down, lockat, rxbytes, rxbytes_at_lastrx, tcalls, tcfix; } st = { "ZMUXSTAT" };

/* 線の処理の排他。Human68k はスーパーバイザの間はスレッドを切り替えないが、Z-OS (zos.x) は自前の横取りで
   スーパーバイザの途中でも切り替えるので、zmux.x のスレッドと npd.x のスレッドと AUX の読み書きが重なりうる
   (重なると受信のバイトが二つに分かれてフレームが壊れた)。取れなければ呼んだ側は何もせずに戻る */
static volatile uint8_t busy;
static int lock(void)
{
  uint16_t sr;
  int ok;
  __asm__ volatile ("move.w %%sr,%0; ori.w #0x0700,%%sr" : "=d"(sr) :: "memory");
  ok = !busy;
  busy = 1;
  if (!ok) st.lockfail++;
  else st.lockat = _iocs_ontime().sec;
  __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
  return ok;
}
static void unlock(void) { busy = 0; }

static long now(void) { return _iocs_ontime().sec; }
static long since(long t) { long d = now() - t; if (d < 0) d += 8640000; return d; }

/* 78125bps (XVI): SCC ch.B の時定数 WR12/WR13 が 0 の構成 (contee78.sys などが 0 にする)。RSDRV.SYS や IOCS が SCC を
   設定し直すと 38400 などに戻り、線が壊れる (切れた後に遅い速さで送り続けた)。組み込むときに 0 だったら、
   10ms に 1 回だけ読み、0 でなければ 0 に書き直す。0 でない構成 (19200 など) では何もしない */
static int tc0;                         /* 組み込みのとき WR12 = WR13 = 0 だった */
static long t_tcchk;

static int scc_tc_read(void)            /* WR12 | WR13 (0 なら 78125bps) */
{
  volatile uint8_t *c = (volatile uint8_t *)0xe98005;
  uint16_t sr;
  uint8_t a, b;
  __asm__ __volatile__("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
  *c = 12; a = *c;
  *c = 13; b = *c;
  __asm__ __volatile__("move.w %0,%%sr" : : "d"(sr) : "cc");
  return a | b;
}

static void scc_tc_guard(void)
{
  volatile uint8_t *c = (volatile uint8_t *)0xe98005;
  uint16_t sr;
  if (!tc0 || since(t_tcchk) < 1)
    return;
  t_tcchk = now();
  if (!scc_tc_read())
    return;
  __asm__ __volatile__("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
  *c = 12; *c = 0;
  *c = 13; *c = 0;
  __asm__ __volatile__("move.w %0,%%sr" : : "d"(sr) : "cc");
  st.tcfix++;
}

static void crcinit(void)
{
  int i, j;
  for (i = 0; i < 256; i++) {
    uint16_t c = i << 8;
    for (j = 0; j < 8; j++)
      c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    crctab[i] = c;
  }
}

static uint16_t crc(uint16_t c, const uint8_t *p, int n)
{
  while (n--)
    c = (c << 8) ^ crctab[(c >> 8) ^ *p++];
  return c;
}

static int ring_count(struct ring *r) { return (r->head - r->tail) & (RINGSZ - 1); }
static int ring_free(struct ring *r) { return RINGSZ - 1 - ring_count(r); }
static void ring_put(struct ring *r, int c) { r->b[r->head] = c; r->head = (r->head + 1) & (RINGSZ - 1); }
static int ring_get(struct ring *r) { int c = r->b[r->tail]; r->tail = (r->tail + 1) & (RINGSZ - 1); return c; }

static int out_free(void) { return OUTSZ - 1 - ((oh - ot) & (OUTSZ - 1)); }
static void out_put(int c) { outq[oh] = c; oh = (oh + 1) & (OUTSZ - 1); }

/* RSDRV.SYS のチャネル 0 の受信バッファを rxbuf に付け替える (slipd.x と同じ) */
static void grow_rsdrv(void)
{
  uint16_t *h = *(uint16_t **)0x170, sr;
  uint8_t *cb;
  if (h[0] != 0x48e7 || h[1] != 0xc0a0 || h[2] != 0x45f9)
    return;
  cb = *(uint8_t **)(h + 3);
  if (*(uint16_t *)(cb + 26) >= RXBUF)
    return;
  __asm__ volatile ("move.w %%sr,%0; ori.w #0x0700,%%sr" : "=d"(sr) :: "memory");
  *(uint8_t **)(cb + 8) = rxbuf;
  *(uint8_t **)(cb + 12) = rxbuf + RXBUF;
  *(uint8_t **)(cb + 16) = rxbuf;
  *(uint8_t **)(cb + 20) = rxbuf;
  *(uint16_t *)(cb + 24) = 0;
  *(uint16_t *)(cb + 26) = RXBUF;
  __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
}

/* フレームを 1 つ線の待ち行列に積む (入らなければ 0) */
static int emit(int chan, int seq, int flags, const uint8_t *d, int len)
{
  uint8_t h[6];
  uint16_t c;
  int i;
  if (out_free() < 2 * (len + 8) + 4) {
    st.refused++;
    return 0;
  }
  h[0] = chan; h[1] = seq; h[2] = rx_expect - 1; h[3] = flags; h[4] = len >> 8; h[5] = len;
  c = crc(0xffff, h, 6);
  c = crc(c, d, len);
  out_put(DLE); out_put(STX);
  for (i = 0; i < 6; i++) { if (h[i] == DLE) out_put(DLE); out_put(h[i]); }
  for (i = 0; i < len; i++) { if (d[i] == DLE) out_put(DLE); out_put(d[i]); }
  h[0] = c >> 8; h[1] = c;
  for (i = 0; i < 2; i++) { if (h[i] == DLE) out_put(DLE); out_put(h[i]); }
  out_put(DLE); out_put(ETX);
  st.txf++;
  need_ack = 0;
  t_last_tx = now();
  return 1;
}

static void send_ctl(int op, int flags)
{
  uint8_t d[4] = { op, VERSION, MAXLEN >> 8, MAXLEN & 255 };
  emit(CH_CTL, 0, flags, d, 4);
}

static void reset_link(void)
{
  tx_seq = tx_base = rx_expect = 0;
  need_ack = nak_pending = 0;
  btx_len = btx_pos = 0;                /* 送りかけの交換は捨てる (npd.x は時間切れで送り直す) */
  brx_len = brx_done = 0;
  rtx_len = rtx_pos = 0;                /* リモートドライブの要求も (zrmt.sys は時間切れで失敗を返す) */
  rd_done = 0;
}

static void go_up(void)
{
  reset_link();
  up = 1;
  st.ups++;
  t_last_rx = t_sent = now();
}

static void go_raw(void)
{
  if (up) {
    st.downs++;
    st.downat = now();
    st.lastrx_at_down = t_last_rx;
  }
  up = 0;
  t_hello = now() - T_HELLO;    /* すぐ HELLO を送る */
}

/* 正しいフレームを受けた */
static void got_frame(void)
{
  int chan = fr[0] & 15, seq = fr[1], ack = fr[2], flags = fr[3], len = (fr[4] << 8) | fr[5];
  uint8_t *d = fr + 6;
  int i;
  st.rxf++;
  if (up)
    t_last_rx = now();                  /* 番号の無いフレーム (KICK など) も、相手が生きていることの知らせにする */
  if (flags & F_UNSEQ) {
    if (chan == CH_CTL && len >= 1) {
      if (d[0] == OP_HELLO) {           /* Pi が始めた (または Pi が作り直した) */
        go_up();
        send_ctl(OP_HELLO_ACK, F_UNSEQ);
      } else if (d[0] == OP_HELLO_ACK) {
        if (!up) go_up();
        else t_last_rx = now();
      } else if (d[0] == OP_BYE) {
        go_raw();
      } else if (d[0] == OP_KICK) {
        kick = 1;
      }
    }
    return;
  }
  if (!up)
    return;
  t_last_rx = now();
  st.rxbytes_at_lastrx = st.rxbytes;
  /* 確認: ack までを窓から外す */
  {
    uint8_t n = ack - tx_base + 1;      /* 外せる数 */
    if (n <= (uint8_t)(tx_seq - tx_base)) {
      tx_base += n;
      t_sent = now();
    }
  }
  if ((flags & F_NAK) && tx_base != tx_seq) {
    t_sent = now() - T_RESEND;          /* すぐ送り直す */
  }
  if (len == 0)
    return;
  if (seq != rx_expect) {
    nak_pending = 1;                    /* 順番が飛んだ / 重複: 確認を返して送り直してもらう */
    need_ack = 1;
    return;
  }
  if (chan == CH_CON) {
    if (ring_free(&rxr) < len) {        /* 受け皿が一杯: 受け取らない (Pi は送り直す) */
      need_ack = 1;
      return;
    }
    for (i = 0; i < len; i++)
      ring_put(&rxr, d[i]);
  } else if (chan == CH_KEY) {
    /* メッセージの列 (フレームの切れ目とメッセージの切れ目は一致しなくてよい)。
       種類の知らないメッセージは長さが分からないので、そのフレームの残りを捨てて次のフレームの頭から読み直す */
    int room = (kt - kh - 1 + KEYQ) % KEYQ;
    if (room < len / 3 + 1) { need_ack = 1; return; }   /* 一杯: 受け取らない (Pi は送り直す) */
    for (i = 0; i < len; i++) {
      if (in_need == 0) {               /* 種類のバイト */
        if (d[i] == IN_KEY) in_need = 2;
        else break;
        in_n = 0;
      } else {
        in_buf[in_n++] = d[i];
        if (--in_need == 0) {
          keyq[kh] = (in_buf[0] << 8) | in_buf[1];
          kh = (kh + 1) % KEYQ;
        }
      }
    }
    if (i < len) { in_need = 0; in_n = 0; }
  } else if (chan == CH_NP) {
    if (brx_done)                       /* 前の応答を npd.x がまだ取っていない: 受け取らない */
      { need_ack = 1; return; }
    if (brx_len + len <= BLK) {
      for (i = 0; i < len; i++)
        brx[brx_len + i] = d[i];
      brx_len += len;
    }
    if (flags & F_END)
      brx_done = 1;
  } else if (chan == CH_RD) {
    if (rd_buf && !rd_done) {           /* 待っている応答。待っていなければ (時間切れの後の遅い応答) 捨てる */
      for (i = 0; i < len && rd_len < rd_max; i++)
        rd_buf[rd_len++] = d[i];
      if (flags & F_END)
        rd_done = 1;
    }
  }
  rx_expect++;
  need_ack = 1;
  t_ack_due = now();
}

/* 線から来た 1 バイト */
static void rx_byte(int c)
{
  switch (pstate) {
  case 0:
    if (c == DLE) { pstate = 1; return; }
    if (!up && ring_free(&rxr) > 0) { ring_put(&rxr, c); st.rawin++; }
    return;
  case 1:
    if (c == STX) { fpos = 0; pstate = 2; return; }
    pstate = 0;
    if (!up && ring_free(&rxr) > 1) { ring_put(&rxr, DLE); ring_put(&rxr, c); st.rawin += 2; }
    return;
  case 2:
    if (c == DLE) { pstate = 3; return; }
    break;
  case 3:
    if (c == DLE) { pstate = 2; break; }
    if (c == STX) { fpos = 0; pstate = 2; return; }
    if (c == ETX) {
      pstate = 0;
      if (fpos >= 8) {
        int len = (fr[4] << 8) | fr[5];
        uint16_t got = (fr[fpos - 2] << 8) | fr[fpos - 1];
        if (len == fpos - 8 && len <= MAXLEN && crc(0xffff, fr, fpos - 2) == got) {
          got_frame();
          return;
        }
      }
      st.bad++;
      if (up) { nak_pending = 1; need_ack = 1; }
      return;
    }
    pstate = 0;                         /* DLE の後に知らないもの: 捨てる */
    st.bad++;
    return;
  }
  if (fpos < (int)sizeof fr)
    fr[fpos++] = c;
  else
    pstate = 0;
}

/* 線を回す: 受けたものを処理し、送るものを作り、待ち行列を線に出す (lock を取ってから) */
static void zl_poll_locked(void)
{
  int n;
  long t = now();
  started = 1;
  if (st.polls && up) {                 /* 線を回す間隔の最大 (1/100 秒)。スレッドが回ってこない時間を調べる */
    long g = t - (long)st.lastpoll;
    if (g < 0) g += 8640000;
    if ((uint32_t)g > st.maxgap) { st.maxgap = g; st.gapat = t; }
  }
  st.polls++;
  st.lastpoll = t;
  scc_tc_guard();
  for (n = 0; n < 512 && _iocs_isns232c(); n++) {
    rx_byte(_iocs_inp232c() & 0xff);
    st.rxbytes++;
  }
  if (up) {
    if (since(t_last_rx) > T_DEAD) {
      go_raw();
    } else {
      /* 送り直し。待ち行列 (outq) に線へ出していないバイトがある間は時計を止める (19200bps で 4KB は 2 秒かかり、
         出してもいないフレームを送り直すと線が重複で埋まる) */
      if (oh != ot)
        t_sent = now();
      if (tx_base != tx_seq && since(t_sent) > T_RESEND) {
        uint8_t s;
        for (s = tx_base; s != tx_seq; s++) {
          int k = s % WIN;
          if (!emit(win[k].chan, s, win[k].flags, win[k].d, win[k].len))
            break;
          st.resend++;
        }
        t_sent = now();
      }
      /* 新しいデータ: チャネル 0 (コンソール) を先に、次にチャネル 3 (交換のブロック)、次にチャネル 2 (リモートドライブ) */
      while ((uint8_t)(tx_seq - tx_base) < WIN && out_free() > 2 * (MAXLEN + 8) + 4 &&
             (ring_count(&txr) > 0 || btx_pos < btx_len || rtx_pos < rtx_len)) {
        int k = tx_seq % WIN, len = 0;
        if (ring_count(&txr) > 0) {
          while (len < MAXLEN && ring_count(&txr) > 0)
            win[k].d[len++] = ring_get(&txr);
          win[k].chan = CH_CON;
          win[k].flags = 0;
        } else if (btx_pos < btx_len) {
          while (len < MAXLEN && btx_pos < btx_len)
            win[k].d[len++] = btx[btx_pos++];
          win[k].chan = CH_NP;
          win[k].flags = btx_pos == btx_len ? F_END : 0;
        } else {
          while (len < MAXLEN && rtx_pos < rtx_len)
            win[k].d[len++] = rtx[rtx_pos++];
          win[k].chan = CH_RD;
          win[k].flags = rtx_pos == rtx_len ? F_END : 0;
        }
        win[k].len = len;
        if (tx_base == tx_seq)
          t_sent = now();
        emit(win[k].chan, tx_seq, win[k].flags, win[k].d, len);
        tx_seq++;
      }
      /* 確認だけのフレーム (生存の知らせも兼ねる) */
      if ((need_ack && since(t_ack_due) >= 1) || nak_pending || since(t_last_tx) > T_KEEP) {
        emit(CH_CON, 0, (nak_pending ? F_NAK : 0) | (ring_free(&rxr) < MAXLEN ? F_BUSY : 0), 0, 0);
        nak_pending = 0;
      }
    }
  }
  if (!up && since(t_hello) > T_HELLO) {
    send_ctl(OP_HELLO, F_UNSEQ);
    t_hello = now();
  }
  /* 線に出す。_OSNS232C が空きを示す分だけ出すと、1 回に 1 バイトほどしか出ず (RSDRV.SYS の送信は 1 バイトずつ)、
     スレッドが回ってくる間隔で速さが決まって 19200bps の半分になった。_OUT232C (空くまで待つ) で 128 バイトまで出す
     (19200bps で約 67ms。その間はほかのスレッドが止まるが、受信は割り込みで RSDRV.SYS のバッファに溜まる) */
  for (n = 0; n < 128 && oh != ot; n++) {
    _iocs_out232c(outq[ot]);
    ot = (ot + 1) & (OUTSZ - 1);
    st.outbytes++;
  }
  st.outq = (oh - ot) & (OUTSZ - 1);
  st.win = (uint8_t)(tx_seq - tx_base) | (need_ack << 8) | (nak_pending << 9) | (up << 10) | (btx_len - btx_pos) << 16;
}

static void zl_poll(void)
{
  if (!lock())
    return;
  zl_poll_locked();
  unlock();
}

static long zmux_call_locked(long cmd, long *a);

long zmux_call(long cmd, long *a)
{
  long r;
  if (cmd == 1)
    return (long)&st;
  if (cmd == 9) {                       /* 時定数 0 (78125bps) の見張りを入れ直す: 今 0 なら見張る */
    tc0 = scc_tc_read() == 0;
    return tc0;
  }
  if (cmd == 0) {
    /* 戻り値: bit0 = UP、bit1 = この回に仕事があった (受信・送信・送り直しのどれか)。常駐スレッドは仕事が無ければ眠る。
       これまでの呼び出し側は 0 / 1 しか見ないので、bit1 は無視される */
    uint32_t w0, w1;
    st.tcalls++;
    if (!up)
      return 0;
    w0 = st.rxbytes + st.outbytes + st.resend + st.txf + st.rxf;
    zl_poll();
    w1 = st.rxbytes + st.outbytes + st.resend + st.txf + st.rxf;
    return up | (w1 != w0 ? 2 : 0);
  }
  if (cmd == 6) {
    /* リモートドライブの交換 (zrmt.sys から): a[0] = 要求, a[1] = 長さ, a[2] = 応答を書く先, a[3] = その最大, a[4] = 待つ時間 (1/100 秒)。
       戻り値 = 応答の長さ。-1 = 線が UP でない、-2 = ほかの要求を送りかけ、-3 = 時間切れ (応答は捨てる)。
       応答が来るまでここで線を回す (AUX の読み込みと同じ。lock は回すたびに取って放すので、ほかのスレッドも線を回せる) */
    const uint8_t *b = (const uint8_t *)a[0];
    long t0;
    int n = a[1], i, r;
    t0 = now();
    while (!lock())                     /* 持ち主が切り替えられて戻ってこない場合 (Z-OS の横取り) に永遠には待たない */
      if (since(t0) > a[4]) return -3;
    if (!up) { unlock(); return -1; }
    if (rtx_pos < rtx_len || n > BLK || n <= 0) { unlock(); return -2; }
    for (i = 0; i < n; i++)
      rtx[i] = b[i];
    rtx_len = n;
    rtx_pos = 0;
    rd_buf = (uint8_t *)a[2];
    rd_max = a[3];
    rd_len = 0;
    rd_done = 0;
    unlock();
    t0 = now();
    for (;;) {
      if (lock()) {
        zl_poll_locked();
        if (rd_done) { r = rd_len; rd_buf = 0; unlock(); return r; }
        if (!up) { rd_buf = 0; unlock(); return -1; }
        if (since(t0) > a[4]) { rd_buf = 0; rtx_len = rtx_pos = 0; unlock(); return -3; }
        unlock();
      }
    }
  }
  if (!lock())                          /* ほかが線を回している: 呼んだ側はあとで呼び直す */
    return cmd == 3 ? 0 : -1;
  r = zmux_call_locked(cmd, a);
  unlock();
  return r;
}

static long zmux_call_locked(long cmd, long *a)
{
  int i;
  /* (命令 0 = スレッドから線を回す、1 = 統計の場所 は zmux_call で。素の文字の間はスレッドは何もしない:
     rx.x などが IOCS で直接読み書きしているかもしれないので、横取りしない) */
  if (cmd == 2) {                       /* 交換の要求を送る: a[0] = ブロック, a[1] = 長さ。0 = 受け付けた、-1 = 線が UP でない / 送りかけ */
    const uint8_t *b = (const uint8_t *)a[0];
    int n = a[1];
    if (!up || btx_pos < btx_len || n > BLK || n <= 0)
      return -1;
    for (i = 0; i < n; i++)
      btx[i] = b[i];
    btx_len = n;
    btx_pos = 0;
    brx_len = 0;
    brx_done = 0;
    kick = 0;
    return 0;
  }
  if (cmd == 3) {                       /* 応答を取る: a[0] = 書く先, a[1] = 最大。戻り値 = 長さ、0 = まだ、-1 = 線が UP でない */
    uint8_t *b = (uint8_t *)a[0];
    int n;
    if (!up)
      return -1;
    if (!brx_done)
      return 0;
    n = brx_len < a[1] ? brx_len : a[1];
    for (i = 0; i < n; i++)
      b[i] = brx[i];
    brx_done = 0;
    brx_len = 0;
    return n;
  }
  if (cmd == 4) {                       /* KICK が来ていたか (読むと消える)。取れなかったら 0 (次に見る) */
    i = kick;
    kick = 0;
    return i;
  }
  if (cmd == 5)
    return up;
  return -1;
}

/* AUX の 1 バイト出力 */
static void con_out(int c)
{
  if (up) {
    while (ring_free(&txr) == 0)
      zl_poll();
    ring_put(&txr, c);
  } else {
    while (out_free() == 0)
      zl_poll();
    out_put(c);
    st.rawout++;
  }
}

long dev_interrupt(struct req_header *req)
{
  extern char _end, __bss_start;
  uint32_t i;
  switch (req->command) {
  case 0: {
    char *b;
    for (b = &__bss_start; b < &_end; b++)   /* CONFIG.SYS から組み込むドライバの BSS は 0 にならない */
      *b = 0;
    st.tag[0] = 'Z'; st.tag[1] = 'M'; st.tag[2] = 'U'; st.tag[3] = 'X';
    st.tag[4] = 'S'; st.tag[5] = 'T'; st.tag[6] = 'A'; st.tag[7] = 'T';
    crcinit();
    tc0 = scc_tc_read() == 0;
    grow_rsdrv();
    old_keysns = (void *)_iocs_b_intvcs(0x101, hk_keysns);   /* IOCS _B_KEYSNS */
    old_keyinp = (void *)_iocs_b_intvcs(0x100, hk_keyinp);   /* IOCS _B_KEYINP */
    t_hello = now() - T_HELLO;
    dos_print("zmux.sys 0.2: AUX (CTTY AUX) is multiplexed on RS-232C by zmux\r\n");
    req->addr = (uint8_t *)&_end;
    return 0;
  }
  case 4:                               /* 入力: 揃うまで戻らない */
    for (i = 0; i < req->status; i++) {
      while (ring_count(&rxr) == 0)
        zl_poll();
      req->addr[i] = ring_get(&rxr);
    }
    return 0;
  case 5:                               /* 1 文字先読み */
    zl_poll();
    req->attr = ring_count(&rxr) ? rxr.b[rxr.tail] : 0;
    return 0;
  case 6:                               /* 入力ステータス: 0 = 読める、1 = 無い */
    zl_poll();
    return ring_count(&rxr) ? 0 : 1;
  case 7:                               /* 入力を捨てる */
    zl_poll();
    rxr.tail = rxr.head;
    return 0;
  case 8:
  case 9:                               /* 出力 */
    for (i = 0; i < req->status; i++)
      con_out(req->addr[i]);
    zl_poll();
    return 0;
  case 10:                              /* 出力ステータス: 0 = 書ける */
    zl_poll();
    return 0;
  case 11:
    return 0;
  case 3:                               /* IOCTRL 入力 / 出力: 何もしない */
  case 12:
    return 0;
  }
  return 0x5003;                        /* 未対応のコマンド */
}
