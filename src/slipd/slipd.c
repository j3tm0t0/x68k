/*
 * slipd.x -- slip.sys とシリアルの間でパケットを運ぶバックグラウンドスレッド (Human68k)
 *
 * ppp.x と同じく、シリアルは IOCS (_ISNS232C / _INP232C / _OSNS232C / _OUT232C) で読み書きし、
 * SCC の割り込み処理は ROM と RSDRV.SYS に任せる。
 *   受信: _INP232C で読んだバイトから SLIP (RFC 1055) をほどき、IPv4 として正しい形のパケットだけを
 *         slip.sys に渡す (ETD 命令 16)。slip.sys が見出しを付けて xip.x の受信ハンドラを呼ぶ。
 *   送信: slip.sys の送信リングから 1 バイトずつ取り出して (ETD 命令 17) _OUT232C で書く。
 *
 *   slipd.x     : 常駐してスレッド "slipd" を作る (2 回目以降は受信の受け入れを再開するだけ)。xip.x の設定 (ifconfig /
 *                 inetdconf) が済んでから起動する (netup がそうする)
 *   必要: CONFIG.SYS の PROCESS 指定 (x68kcli は PROCESS = 16 10 50)
 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdint.h>

extern long call_etd(void *entry, long cmd, void *arg);
extern char *mem_ptr;
extern unsigned char *cmd_line;

#define MTU     1500
#define BURST   512                     /* 1 周で読み書きする最大バイト数 (ほかのスレッドに譲るため) */

static void *etd;                       /* slip.sys の ETD 入口 */
/* 動作確認用のカウンタ (メモリダンプで "SLPDSTAT" の後ろを読む) */
static struct { char tag[8]; uint32_t loops, rx, tx, bad; } stat = { "SLPDSTAT" };
static struct dos_prcctrl prc;           /* スレッドのメッセージ受け箱 (xip.x と同じく 4 バイト) */
static unsigned char mbox[4];
/* xip.x の受信ハンドラはこの sstack の上で動き、TCP の処理から応答の送信 (slip.sys) まで呼ぶので深い。
 * 4KB では溢れて slipd 自身のコードと Human68k のメモリ管理情報を壊し、gcc / make / zsh が落ちた */
#define SSTACK  32768
static char ustack[4096] __attribute__((aligned(4)));
static uint32_t sstack[SSTACK / 4];
/* 受信中のパケット。先頭 14 バイトは slip.sys が Ethernet の見出しを書く余白 */
static uint8_t frame[14 + MTU] __attribute__((aligned(4)));

/* RSDRV.SYS の受信バッファ (1KB) の差し替え先。38400 bps だと 1KB は 0.27 秒分しかなく、xip.x の TCP 処理などで
 * slipd が少し待たされるだけで溢れてパケットが壊れた (XEiJ で 20KB の転送が 9600 bps より遅くなった) */
#define RXBUF   32768
static uint8_t rxbuf[RXBUF];

/* RSDRV.SYS 1.13 のチャネル 0 (内蔵 RS-232C) の受信バッファを rxbuf に付け替える。
 * 受信割り込み ($170) の入口が「movem.l d0-d1/a0/a2,-(sp); lea 制御ブロック,a2」の形のときだけ。
 * 制御ブロック: +8 バッファ先頭, +12 バッファ末尾, +16 書き込み位置, +20 読み出し位置, +24 個数.w, +26 大きさ.w */
static int grow_rsdrv(void)
{
  uint16_t *h = *(uint16_t **)0x170, sr;
  uint8_t *cb;
  if (h[0] != 0x48e7 || h[1] != 0xc0a0 || h[2] != 0x45f9)
    return 0;
  cb = *(uint8_t **)(h + 3);
  if (*(uint16_t *)(cb + 26) >= RXBUF)
    return 0;
  __asm__ volatile ("move.w %%sr,%0; ori.w #0x0700,%%sr" : "=d"(sr) :: "memory");
  *(uint8_t **)(cb + 8) = rxbuf;
  *(uint8_t **)(cb + 12) = rxbuf + RXBUF;
  *(uint8_t **)(cb + 16) = rxbuf;
  *(uint8_t **)(cb + 20) = rxbuf;
  *(uint16_t *)(cb + 24) = 0;              /* 残っていた受信データは捨てる */
  *(uint16_t *)(cb + 26) = RXBUF;
  __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
  return 1;
}

static int memeq(const void *a, const void *b, int n)
{
  const uint8_t *x = a, *y = b;
  while (n--)
    if (*x++ != *y++)
      return 0;
  return 1;
}

/* デバイスチェインから "/dev/ep0" + "EthD" + "SLIP" を探す (ESP/X libether.c と同じ方法) */
static void *find_slip(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "/dev/ep0", 8) && memeq(d + 22, "EthDSLIP", 8))
          return d + 30;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* SLIP (RFC 1055) を 1 バイトずつほどき、IPv4 として正しい形のパケットだけを slip.sys に渡す */
static void slip_in(int b)
{
  static int pos, esc, bad;
  if (b == 0xc0) {                      /* END: パケットの区切り */
    uint8_t *ip = frame + 14;
    /* IPv4 として正しい形のものだけ渡す (版 = 4, ヘッダ長 >= 20, 全長 = 受け取った長さ)。
       Linux の sl0 は IPv6 (ルータ要請など) も送ってくることがあり、xip.x に IPv4 として渡すと壊れた */
    if (pos >= 20 && !bad && (ip[0] >> 4) == 4 && (ip[0] & 15) >= 5 && ((ip[2] << 8) | ip[3]) == pos) {
      long arg[2];
      arg[0] = pos;
      arg[1] = (long)frame;
      if (call_etd(etd, 16, arg) > 0)
        stat.rx++;
    } else if (pos)
      stat.bad++;
    pos = esc = bad = 0;
    return;
  }
  if (b == 0xdb) {                      /* ESC */
    esc = 1;
    return;
  }
  if (esc) {
    b = b == 0xdc ? 0xc0 : b == 0xdd ? 0xdb : b;
    esc = 0;
  }
  if (pos < MTU)
    frame[14 + pos++] = b;
  else
    bad = 1;
}

/* スレッド本体 (スーパーバイザモード: slip.sys を DOS を通さずに直接呼べ、途中でスレッドが切り替わらない)。
 * 読み書きするものが無ければ _CHANGE_PR で譲る (_SLEEP_PR で眠ると、そのうち起こされなくなった) */
static void thread(void)
{
  for (;;) {
    int n, did = 0;
    long c;
    stat.loops++;
    for (n = 0; n < BURST && _iocs_isns232c(); n++) {
      slip_in(_iocs_inp232c() & 0xff);
      did = 1;
    }
    for (n = 0; n < BURST && _iocs_osns232c() && (c = call_etd(etd, 17, 0)) >= 0; n++) {
      _iocs_out232c(c);
      stat.tx++;
      did = 1;
    }
    if (!did)
      _dos_change_pr();
  }
}

/* -z: シリアルの代わりに x68kzremotedrv (Raspberry Pi Pico W) の SCSI ID 6 で運ぶ (netpipe)。
 * 1 回の交換で、要求 (見出し 16 バイト + 送信データ) を ZWLBA に書き、応答 (見出し + 受信データ) を ZRLBA 帯から読む。
 * 応答を読む位置は毎回 8 ずつ下げる (X68000 Z 側でディスクの読み込みがキャッシュされるため。scsiremote.sys と同じ方法) */
#define ZID     6
#define ZWLBA   0x100000
#define ZRLBA   0x180000
#define ZRSPAN  0x80000
#define ZSECTS  8
#define ZDATA   (ZSECTS * 512 - 16)
#define ZMAGIC  0x58364e50              /* "X6NP" */
#define ZIDLE   2                       /* 何も無いときの問い合わせ間隔 (1/100 秒) */

struct zhdr { uint32_t magic, seq; uint16_t len, more; uint32_t stat; };
static uint8_t zreq[ZSECTS * 512] __attribute__((aligned(4)));
static uint8_t zres[ZSECTS * 512] __attribute__((aligned(4)));
static uint32_t zseq;
static int zsect = ZRLBA;
static struct { char tag[8]; uint32_t xchg, retry, err, drops; } zstat = { "SLPDZSTA" };
/* 調査用のトレース (交換ごとに 時刻 1/100 秒, 送信バイト, 受信バイト, 渡したパケット数)。"SLPDTRAC" の後ろ: 位置, 記録 256 件 */
#define ZTRACE  256
static struct { char tag[8]; uint32_t pos; struct { int32_t t; uint16_t tx, rx; uint16_t pk, pad; } e[ZTRACE]; } ztrace = { "SLPDTRAC" };

static int zstep(int d)
{
  zsect = ZRLBA + ((zsect - ZRLBA - d) & (ZRSPAN - 8));
  return zsect;
}

/* 1 回交換する。戻り値: 受信データ長 (応答の more に残りの長さ), 失敗なら -1 */
static int zxchg(int txlen, int *more)
{
  struct zhdr *q = (struct zhdr *)zreq, *r = (struct zhdr *)zres;
  int try, n;
  q->magic = ZMAGIC;
  q->seq = ++zseq;
  q->len = txlen;
  q->more = ZDATA;
  q->stat = 0;
  if (_iocs_s_writeext(ZWLBA, (16 + txlen + 511) / 512, ZID, 1, zreq) != 0)
    return -1;
  for (try = 0; ; try++) {
    if (_iocs_s_readext(zstep(8), 1, ZID, 1, zres) != 0)
      return -1;
    if (r->magic == ZMAGIC && r->seq == zseq)
      break;
    zstat.retry++;
    if (try >= 3)
      return -1;
    zstep(0x10000 - 8);                 /* キャッシュに残っていた: 大きく飛ばす */
  }
  n = r->len;
  if (n > ZDATA)
    return -1;
  if (16 + n > 512 && _iocs_s_readext(zsect + 1, (16 + n - 1) / 512, ZID, 1, zres + 512) != 0)
    return -1;
  zstat.xchg++;
  zstat.drops = r->stat;
  *more = r->more;
  return n;
}

static int ticks(void)
{
  return _iocs_ontime().sec;            /* 1/100 秒 (0 時に戻る) */
}

static void zthread(void)
{
  int last = 0, hot = 0;
  for (;;) {
    int n = 0, now, r, more = 0, i;
    long c;
    stat.loops++;
    while (n < ZDATA && (c = call_etd(etd, 17, 0)) >= 0)
      zreq[16 + n++] = c;
    now = ticks();
    if (n == 0 && hot == 0 && (unsigned)(now - last) < ZIDLE) {
      _dos_change_pr();
      continue;
    }
    last = now;
    r = zxchg(n, &more);
    if (r < 0) {
      zstat.err++;
      while ((unsigned)(ticks() - now) < 100)   /* Pico が居ない・つながらない: 1 秒待つ */
        _dos_change_pr();
      continue;
    }
    stat.tx += n;
    {
      uint32_t rx0 = stat.rx;
      for (i = 0; i < r; i++)
        slip_in(zres[16 + i]);
      if (n || r) {
        int k = ztrace.pos++ % ZTRACE;
        ztrace.e[k].t = now;
        ztrace.e[k].tx = n;
        ztrace.e[k].rx = r;
        ztrace.e[k].pk = stat.rx - rx0;
      }
    }
    /* やり取りがあった直後は間を置かずに問い合わせる (TCP の応答や続きがすぐ来る) */
    hot = more ? 1 : (n || r) ? 20 : hot ? hot - 1 : 0;
    _dos_change_pr();
  }
}

int slipd_main(void)
{
  int ssp, zmode = 0;
  {
    unsigned char *p = cmd_line + 1;     /* 先頭 1 バイトは長さ */
    for (; *p; p++)
      if (p[0] == '-' && (p[1] | 0x20) == 'z')
        zmode = 1;
  }
  ssp = _dos_super(0);
  etd = find_slip();
  if (!etd) {
    if (ssp > 0) _dos_super(ssp);
    _dos_print("slipd: slip.sys (/dev/ep0) が組み込まれていません\r\n");
    return 1;
  }
  if (call_etd(etd, 18, 0)) {          /* スレッドは常駐済み (受け入れの再開だけ) */
    if (ssp > 0) _dos_super(ssp);
    _dos_print("slipd: 受信を再開しました (常駐済み)\r\n");
    return 0;
  }
  if (!zmode && !grow_rsdrv())
    _dos_print("slipd: RSDRV.SYS の受信バッファを広げられません (1KB のまま)\r\n");
  if (ssp > 0) _dos_super(ssp);
  prc.length = sizeof mbox;
  prc.buf_ptr = mbox;
  prc.your_id = 0xffff;                 /* 受け箱は空 (0 のままだと「使用中」とみなされ _SEND_PR が届かない) */
  {
    /* スリープ時間 0 で作ったスレッドはメッセージが届くまで眠ったまま (wait_flg = 0xff) なので、
       作ったらすぐ 1 通送って起こす (xip.x も _SEND_PR でスレッドを起こしている) */
    int tid = _dos_open_pr("slipd", 2, (int)(ustack + sizeof ustack), (int)((char *)sstack + sizeof sstack),
                           0x2000, (int)(zmode ? zthread : thread), &prc, 0);
    static char msg[4];
    static struct dos_prcptr me;        /* _GET_PR は情報をここに書く (NULL を渡すとアドレス 0 を壊す) */
    int myid, r;
    if (tid < 0) {
      _dos_print("slipd: スレッドを作れません (CONFIG.SYS の PROCESS を確認)\r\n");
      return 1;
    }
    myid = _dos_get_pr(-1, &me);
    while ((r = _dos_send_pr(myid, tid, 0, msg, sizeof msg)) == -28)   /* 使用中なら譲ってから送り直す (xip.x と同じ) */
      _dos_change_pr();
    if (r < 0)
      _dos_print("slipd: スレッドを起こせません\r\n");
  }
  _dos_print(zmode ? "slipd: SLIP thread started (SCSI ID 6 netpipe)\r\n" : "slipd: SLIP thread started\r\n");
  {
    /* 常駐する大きさは BSS (スレッドのスタック) まで含める。a1 (prg_end) だと BSS が入らず、
       解放された領域をスレッドが使い続けて gcc などが壊れた */
    extern char _end;
    _dos_keeppr(&_end - (mem_ptr + 0x100), 0);
  }
  return 0;
}
