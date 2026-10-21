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
 * (Pico W の netpipe のブリッジを SLIP なしで速く運ぶのは net/npd の npd.sys / npd.x)
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
/* 送受信したパケットの先頭を残す ("SLPDTAP_"、ztap.x で表示)。どこで消えているかを Z の上で見るため */
#define ZTAP    64
static struct { char tag[8]; uint32_t pos; struct { int32_t t; uint8_t dir, pad; uint16_t len; uint8_t b[28]; } e[ZTAP]; } ztap = { "SLPDTAP_" };

static int ticks(void);

static void tap(int dir, const uint8_t *p, int len)
{
  int k = ztap.pos++ % ZTAP, i;
  ztap.e[k].t = ticks();
  ztap.e[k].dir = dir;
  ztap.e[k].len = len;
  for (i = 0; i < 28; i++)
    ztap.e[k].b[i] = i < len ? p[i] : 0;
}

/* 送る SLIP バイト列をほどいて、1 パケットごとに tap する */
static void tap_tx(int b)
{
  static uint8_t pk[28];
  static int pos, esc;
  if (b == 0xc0) {
    if (pos)
      tap('T', pk, pos);
    pos = esc = 0;
    return;
  }
  if (b == 0xdb) {
    esc = 1;
    return;
  }
  if (esc) {
    b = b == 0xdc ? 0xc0 : b == 0xdd ? 0xdb : b;
    esc = 0;
  }
  if (pos < 28)
    pk[pos] = b;
  pos++;
}

static int pos, esc, bad;                /* SLIP をほどいている途中の状態 (slip_in と slip_in_buf で共有) */

static void slip_in(int b)
{
  if (b == 0xc0) {                      /* END: パケットの区切り */
    uint8_t *ip = frame + 14;
    /* IPv4 として正しい形のものだけ渡す (版 = 4, ヘッダ長 >= 20, 全長 = 受け取った長さ)。
       Linux の sl0 は IPv6 (ルータ要請など) も送ってくることがあり、xip.x に IPv4 として渡すと壊れた */
    if (pos >= 20 && !bad && (ip[0] >> 4) == 4 && (ip[0] & 15) >= 5 && ((ip[2] << 8) | ip[3]) == pos) {
      long arg[2];
      arg[0] = pos;
      arg[1] = (long)frame;
      tap('R', ip, pos);
      if (call_etd(etd, 16, arg) > 0)
        stat.rx++;
    } else if (pos) {
      tap('B', ip, pos);
      stat.bad++;
    }
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

/* まとめて受けた SLIP をほどく。ふつうのバイトはその場で写し、END / ESC とその次のバイトだけ slip_in に回す。
 * 1 バイトずつ slip_in を呼ぶと、netpipe で受けるたびに CPU の 1 割強をこれに使っていた (68000 では関数呼び出しが重い) */
static void slip_in_buf(const uint8_t *s, int n)
{
  uint8_t *f = frame + 14;
  while (n > 0) {
    int b = *s;
    if (b == 0xc0 || b == 0xdb || esc) {
      slip_in(b);
      s++;
      n--;
      continue;
    }
    {
      int k = 0;
      while (k < n && s[k] != 0xc0 && s[k] != 0xdb)
        k++;
      if (pos + k > MTU) {
        bad = 1;
        k = k < MTU - pos ? k : (pos < MTU ? MTU - pos : 0);
        if (k == 0) {
          s++;
          n--;
          continue;
        }
      }
      {
        uint8_t *d = f + pos;             /* -nostdlib なので memcpy は使えない */
        int j;
        for (j = 0; j < k; j++)
          d[j] = s[j];
      }
      pos += k;
      s += k;
      n -= k;
    }
  }
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
/* 応答を読む帯。Z 側の Linux は読み込みをキャッシュするので、帯が狭いと一巡したところで古い応答が返り、
   それきり交換できなくなった (0x80000 セクタの帯で 65536 回 = 20ms 間隔で約 22 分、2026-10-21)。
   約 480MB (0x110000～0x1ffff7) を回る (ファームの NETPIPE_RLBA / NETPIPE_REND と同じ)。
   0x200000 以降は scsiremote.sys が使う */
#define ZRLBA   0x110000
#define ZRSPAN  0xf0000
#define ZSECTS  8
#define ZDATA   (ZSECTS * 512 - 16)
#define ZMAGIC  0x58364e50              /* "X6NP" */
#define ZIDLE   6                       /* やり取りの直後の問い合わせ間隔 (1/100 秒、-i で変えられる) */
#define ZIDLE2  5                       /* 2 秒やり取りが無ければこの間隔に延ばす (帯を回る速さも落とす) */

struct zhdr { uint32_t magic, seq; uint16_t len, more; uint32_t stat; };
static uint8_t zreq[ZSECTS * 512] __attribute__((aligned(4)));
static uint8_t zres[ZSECTS * 512] __attribute__((aligned(4)));
static uint32_t zseq;
static uint32_t zmyip;                  /* -z の後ろの IP アドレス。0 でなければ Pico がブリッジする (Z が LAN 上の 1 台になる) */
static int zsect = ZRLBA;
static int zidle = ZIDLE;
static struct { char tag[8]; uint32_t xchg, retry, err, drops; } zstat = { "SLPDZSTA" };
/* 調査用のトレース (交換ごとに 時刻 1/100 秒, 送信バイト, 受信バイト, 渡したパケット数)。"SLPDTRAC" の後ろ: 位置, 記録 256 件 */
#define ZTRACE  256
static struct { char tag[8]; uint32_t pos; struct { int32_t t; uint16_t tx, rx; uint16_t pk, pad; } e[ZTRACE]; } ztrace = { "SLPDTRAC" };

static int zstep(int d)
{
  zsect = ZRLBA + (unsigned)(zsect - ZRLBA + ZRSPAN - d) % ZRSPAN;   /* d は 8 の倍数で ZRSPAN 未満 */
  return zsect;
}

/* 1 回交換する。戻り値: 受信データ長 (応答の more に残りの長さ), 失敗なら -1 */
/* first: 最初の読み込みのセクタ数。データが続いているときは 8 (一度に読み切る)、静かなときは 1。
 * Z の SCSI は Pico との USB のやり取りが終わるまで ROM が待ち続けるので、1 コマンドが数 ms の CPU になる。
 * バイト数よりコマンドの回数を減らす方が効く */
/* SCSI の読み書きを、ROM (Pseudo SCSI BIOS) を通さずメールボックスで行う。ROM は命令を出したあと、Pico との
 * USB のやり取りが終わるまで nop で空回りする (プロファイルでは CPU の 25% 以上)。ここでは終わるまで
 * _CHANGE_PR で xip.x などに譲る。メールボックス: +0 bit7 使用中, +1 段階 (4 で開始), +2 ID, +3 SCSI 命令,
 * +4 LBA, +8 バイト数, +12 バッファ, +16/+18 状態。ほかの SCSI の呼び出しは start.S の scsi_hook が待たせる */
#define MB ((volatile uint8_t *)0xea0060)
extern volatile uint16_t scsi_inrom;
extern volatile uint8_t scsi_zown;      /* slipd.x の命令が終わって段階 (+1) を 0 に戻すまでの間 1 */
static int zio(int op, int lba, int nsect, void *buf)
{
  int sr, r;
  for (;;) {
    __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
    /* B: の scsiremote.sys などは、選択・命令・データ・状態を別々の IOCS で呼ぶ。その合間 (ROM の外) でも
       段階 (+1) は 0 でないので、それも待つ */
    if (!scsi_inrom && !(MB[0] & 0x80) && !(MB[1] & 15))
      break;
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
    _dos_change_pr();
  }
  MB[2] = ZID;
  MB[3] = op;
  *(volatile uint32_t *)(MB + 4) = lba;
  *(volatile uint32_t *)(MB + 8) = nsect * 512;
  *(volatile uint32_t *)(MB + 12) = (uint32_t)buf;
  MB[0] = 0x80;
  MB[1] = 4;
  scsi_zown = 1;
  __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
  while (MB[0] & 0x80)
    _dos_change_pr();
  /* 終わったあと、ほかの SCSI の呼び出しが先に段階を 0 に戻していたら (scsi_hook)、状態はもう分からない。
     成功とみなす (読み込みは番号で確かめるので、失敗していても読み直しになるだけ) */
  r = 0;
  __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
  if (scsi_zown) {
    MB[1] = 0;
    scsi_zown = 0;
    r = *(volatile uint16_t *)(MB + 16) | *(volatile uint16_t *)(MB + 18);
  }
  __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
  return r ? -1 : 0;
}
#define zread(lba, n, buf)  zio(0x28, lba, n, buf)
#define zwrite(lba, n, buf) zio(0x2a, lba, n, buf)

static int zxchg(int txlen, int *more, int first)
{
  struct zhdr *q = (struct zhdr *)zreq, *r = (struct zhdr *)zres;
  int try, n;
  q->magic = ZMAGIC;
  q->seq = ++zseq;
  q->len = txlen;
  q->more = ZDATA;
  q->stat = zmyip;
  if (zwrite(ZWLBA, (16 + txlen + 511) / 512, zreq) != 0)
    return -1;
  for (try = 0; ; try++) {
    if (zread(zstep(8), first, zres) != 0)
      return -1;
    if (r->magic == q->magic && r->seq == zseq)
      break;
    zstat.retry++;
    if (try >= 64)
      return -1;
    /* キャッシュに残っていた古い応答: 16MB 飛ばして読み直す。リセットを繰り返すと、前の回までに読んだ範囲
       (100MB を超えることもある) がキャッシュに残っていて、3 回で諦めると抜け出せず交換が全滅した。
       キャッシュに当たる読み込みは USB を通らないので、何度読み直しても安い */
    zstep(0x8000 - 8);
  }
  n = r->len;
  if (n > ZDATA)
    return -1;
  if (16 + n > first * 512 && zread(zsect + first, (16 + n - 1) / 512 + 1 - first, zres + first * 512) != 0)
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
  int last = 0, hot = 0, act = 0;
  for (;;) {
    int n = 0, now, r, more = 0;
    long c;
    stat.loops++;
    while (n < ZDATA && (c = call_etd(etd, 17, 0)) >= 0) {
      zreq[16 + n++] = c;
      tap_tx(c & 0xff);
    }
    now = ticks();
    if (n == 0 && hot == 0 && (unsigned)(now - last) < ((unsigned)(now - act) < 200 ? zidle : ZIDLE2)) {
      _dos_change_pr();
      continue;
    }
    last = now;
    r = zxchg(n, &more, (unsigned)(now - act) < 20 ? ZSECTS : 1);
    if (r < 0) {
      zstat.err++;
      while ((unsigned)(ticks() - now) < 100)   /* Pico が居ない・つながらない: 1 秒待つ */
        _dos_change_pr();
      continue;
    }
    stat.tx += n;
    {
      uint32_t rx0 = stat.rx;
      slip_in_buf(zres + 16, r);
      if (n || r) {
        int k = ztrace.pos++ % ZTRACE;
        act = now;
        ztrace.e[k].t = now;
        ztrace.e[k].tx = n;
        ztrace.e[k].rx = r;
        ztrace.e[k].pk = stat.rx - rx0;
      }
    }
    /* Pico に続きがあるときだけ間を置かずに問い合わせる。送るものができたら (TCP の ACK など) その場で交換する。
       以前はやり取りの直後に 20 回続けて問い合わせていたが、空の交換 (SCSI の読み書き、1 回数 ms) が
       CPU を食って xip.x の番が回らず、パケット 1 個に 6〜7 回交換していた */
    hot = more ? 1 : 0;
    _dos_change_pr();
  }
}

int slipd_main(void)
{
  int ssp, zmode = 0;
  {
    unsigned char *p = cmd_line + 1;     /* 先頭 1 バイトは長さ */
    for (; *p; p++)
      if (p[0] == '-' && (p[1] | 0x20) == 'i' && p[2] >= '0' && p[2] <= '9') {
        /* -i N: やり取りの直後の問い合わせ間隔 (1/100 秒)。短いほど応答は速いが、空の交換が CPU を食う */
        zidle = 0;
        for (p += 2; *p >= '0' && *p <= '9'; p++)
          zidle = zidle * 10 + (*p - '0');
        p--;
      } else if (p[0] == '-' && (p[1] | 0x20) == 'z') {
        /* -z a.b.c.d: ブリッジ (Pico が Wi-Fi の LAN で a.b.c.d を名乗る)。-z だけなら UDP で Pi と運ぶ */
        unsigned char *q = p + 2;
        uint32_t ip = 0, v = 0;
        int dots = 0, digits = 0;
        zmode = 1;
        while (*q == ' ')
          q++;
        for (; (*q >= '0' && *q <= '9') || *q == '.'; q++) {
          if (*q == '.') {
            ip = (ip << 8) | v;
            v = digits = 0;
            dots++;
          } else {
            v = v * 10 + (*q - '0');
            digits++;
          }
        }
        if (dots == 3 && digits && v < 256)
          zmyip = (ip << 8) | v;
      }
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
  if (zmode) {
    /* 番号と読み始める位置を毎回変える。Z 側の読み込みキャッシュは X68000 をリセットしても残るので、
       1 から数え直すと、前の回の応答 (番号も合う) を読んでしまい、古いパケットが流れ込んだ */
    uint32_t h = ((uint32_t)_dos_getdate() << 17) ^ (uint32_t)_dos_gettim2() ^ ((uint32_t)_iocs_ontime().sec << 7);
    h *= 2654435761u;
    zseq = h;
    zsect = ZRLBA + (h >> 8) % (ZRSPAN / 8) * 8;
  }
  if (zmode) {
    /* SCSI の入口にフックを挟む (zio を参照)。スレッドを作る前に、スーパーバイザで */
    extern void scsi_hook(void);
    extern uint32_t scsi_old;
    volatile uint32_t *vec = (volatile uint32_t *)(0x400 + 0xf5 * 4);
    int s = _dos_super(0);
    scsi_old = *vec;
    *vec = (uint32_t)scsi_hook;
    if (s > 0) _dos_super(s);
  }
  if (zmode) {
    /* 簡易プロファイラ (start.S の prof_vdisp、zprof.x で使う)。垂直同期の割り込みが空いていなければ使わない */
    extern void prof_vdisp(void);
    _iocs_vdispst(prof_vdisp, 0, 1);
  }
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
  _dos_print(!zmode ? "slipd: SLIP thread started\r\n" : zmyip ? "slipd: SLIP thread started (SCSI ID 6 netpipe, bridge)\r\n"
                    : "slipd: SLIP thread started (SCSI ID 6 netpipe, UDP)\r\n");
  {
    /* 常駐する大きさは BSS (スレッドのスタック) まで含める。a1 (prg_end) だと BSS が入らず、
       解放された領域をスレッドが使い続けて gcc などが壊れた */
    extern char _end;
    _dos_keeppr(&_end - (mem_ptr + 0x100), 0);
  }
  return 0;
}
