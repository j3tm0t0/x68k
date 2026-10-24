/*
 * npd.x -- npd.sys と x68kzremotedrv (Raspberry Pi Pico W) の netpipe の間でパケットを運ぶバックグラウンドスレッド
 *
 * Pico W の SCSI ID 6 の空き LBA を使い、1 回の交換で要求 (見出し 16 バイト + 送信データ) を ZWLBA に書き、
 * 応答 (見出し + 受信データ) を ZRLBA 帯から読む。データは IP パケットを長さ付きで並べたレコード ("X6NR")。
 * Pico は Z の IP アドレスを Wi-Fi の LAN で名乗り (代理 ARP)、Z 宛ての IPv4 を渡し、Z からのものを LAN に出す。
 *   受信: 応答のレコードを 1 個ずつ npd.sys に渡す (ETD 命令 16)。npd.sys が見出しを付けて xip.x の受信ハンドラを呼ぶ。
 *   送信: npd.sys の送信リングからレコードをまとめて取り出す (ETD 命令 19)。
 *   中身の先頭が ACCEL_MARK (0xA5) のレコードは暗号アクセラレータ (net/accel) の要求 / 応答。要求は npd.sys が
 *   ETD 命令 19 の出力の後ろに載せ、応答は ETD 命令 23 で npd.sys に渡す (Pico が X25519 / Ed25519 を計算する)。
 *   中身の先頭が TSP_MARK (0xA6) のレコードは tis.x -p (net/tistack) のソケット要求 / 応答。tis.x が npd.sys に
 *   預けた入口 (ETD 命令 26) を呼んで、交換ごとに 1 個作ってもらい (TIS_NP_OUT)、応答を渡す (TIS_NP_IN)。
 *   中身の先頭が ZSS_MARK (0xA7) のレコードは Pico の zsshd と zos.x (x68k-zos) の間の端末のセッション (zss.h)。
 *   TSP と同じ形で、zos.x が預けた入口 (ETD 命令 28) を ZSS_NP_OUT / ZSS_NP_IN で呼ぶ。
 *
 *   npd.x -m a.b.c.d    : SCSI ID 6 の代わりに zmux (net/zmux) の線のチャネル 1 で交換する (実機 X68000 と Raspberry Pi の
 *                         RS-232C)。zmux.sys と zmux.x が要る。送るものが無いときは交換せず、Pi の KICK で交換する
 *   npd.x [-iN] a.b.c.d : 常駐してスレッド "npd" を作る (2 回目以降は受信の受け入れを再開するだけ)。a.b.c.d は Z の
 *                         IP アドレス。xip.x の設定 (ifconfig / inetdconf) が済んでから起動する
 *   必要: CONFIG.SYS の DEVICE = \SYS\npd.sys と PROCESS 指定、netpipe 入りの Pico W のファーム
 *   調べる道具 (harness/ztools) のために、カウンタなどの印は slipd.x と同じ ("SLPDSTAT" "SLPDZSTA" ...)
 *
 * (RS-232C の SLIP と、Pi の slattach を相手にする netpipe の UDP モードは net/slip, net/slipd)
 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdint.h>
#include "../accel/accel_proto.h"
#include "../tistack/tsp.h"
#include "zss.h"

extern long call_etd(void *entry, long cmd, void *arg);
extern char *mem_ptr;
extern unsigned char *cmd_line;

static void *etd;                       /* npd.sys の ETD 入口 */
static void *mux;                       /* -m: zmux.sys の入口 (d0 = 命令, a0 = 引数。call_etd で呼べる) */
static void *tis;                       /* tis.x -p の入口 (交換のたびに npd.sys から取り直す)、0 = 居ない */
static int tsp_more;                    /* Pico の TSP 応答に続きがある */
static void *zss;                       /* zos.x の 0xA7 の入口 (交換のたびに取り直す)、0 = 居ない */
/* 0xA7 の新しい要求の上限: 半分ずつの交換で TSP が使う残り ((ZDATA - n) / 2 - 2、n = 0) のあとに残る場所と同じ。
   これより大きい要求は、答えが来なかったときの送り直しが半分ずつの交換に入らない */
#define ZSS_CAP ((ZDATA / 2 - 2) & ~1)
static int share_tsp;                   /* TSP と 0xA7 がどちらも急ぐとき、この交換は TSP に場所を (1) / 半分ずつ (0)。交互に替える */

/* zos.x (常駐しない、ふつうのプログラム) の入口を呼んでよいか: 入口の前に "ZSSE" があり、それを含むメモリのブロックが
   まだ確保されている (Human68k のメモリ管理の鎖 [$1C20] から +$0C でたどり、+0 の次 〜 +8 の終わりの中)。zos.x が
   入口を外さずに終わった (落ちた) ときに、解放されたところを呼ばないため */
static void *zss_check(void *e)
{
  uint8_t *a = e, *m;
  int k;
  if (!e || e == (void *)-1 || ((uint32_t)a & 1))
    return 0;
  for (m = *(uint8_t **)0x1c20, k = 0; m && k < 4096; m = *(uint8_t **)(m + 12), k++)   /* 先に鎖を見てから "ZSSE" を読む */
    if (a >= m + 16 + 4 && a < *(uint8_t **)(m + 8))
      return *(uint32_t *)(a - 4) == 0x5a535345 ? e : 0;
  return 0;
}
/* 動作確認用のカウンタ (メモリダンプで "SLPDSTAT" の後ろを読む) */
static struct { char tag[8]; uint32_t loops, rx, tx, bad; } stat = { "SLPDSTAT" };
static struct dos_prcctrl prc;           /* スレッドのメッセージ受け箱 (xip.x と同じく 4 バイト) */
static unsigned char mbox[4];
/* xip.x の受信ハンドラはこの sstack の上で動き、TCP の処理から応答の送信 (npd.sys) まで呼ぶので深い */
#define SSTACK  32768
static char ustack[4096] __attribute__((aligned(4)));
static uint32_t sstack[SSTACK / 4];

static int memeq(const void *a, const void *b, int n)
{
  const uint8_t *x = a, *y = b;
  while (n--)
    if (*x++ != *y++)
      return 0;
  return 1;
}

/* デバイスチェインから "/dev/ep0" + "EthD" + "NPD " を探す (ESP/X libether.c と同じ方法) */
static void *find_npd(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "/dev/ep0", 8) && memeq(d + 22, "EthDNPD ", 8))
          return d + 30;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

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

/* デバイスチェインから zmux.sys ("AUX     " + "ZMUX") を探す。入口はヘッダ +0x1a */
static void *find_zmux(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "AUX     ", 8) && memeq(d + 22, "ZMUX", 4))
          return d + 26;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* netpipe: Pico W の SCSI ID 6 で運ぶ。応答を読む位置は毎回 8 ずつ下げる (X68000 Z 側でディスクの読み込みが
 * キャッシュされるため。scsiremote.sys と同じ方法) */
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
#define ZMAGIC  0x58364e52              /* "X6NR": IP パケットを長さ付きのレコードで運ぶ (Pico はブリッジする)。
                                           Z へ: [長さ 2][空き 14][IP][詰め物] / Z から: [長さ 2][IP][詰め物] */
#define ZIDLE   6                       /* やり取りの直後の問い合わせ間隔 (1/100 秒、-i で変えられる) */
#define ZIDLE2  5                       /* 2 秒やり取りが無ければこの間隔に延ばす (帯を回る速さも落とす) */

struct zhdr { uint32_t magic, seq; uint16_t len, more; uint32_t stat; };
static uint8_t zreq[ZSECTS * 512] __attribute__((aligned(4)));
static uint8_t zres[ZSECTS * 512] __attribute__((aligned(4)));
static uint32_t zseq;
static uint32_t zmyip;                  /* Z の IP アドレス。Pico がこれを Wi-Fi の LAN で名乗る (Z が LAN 上の 1 台になる) */
static int zsect = ZRLBA;
static int zidle = ZIDLE;
static struct { char tag[8]; uint32_t xchg, retry, err, drops; } zstat = { "SLPDZSTA" };
/* 交換の失敗の内訳 (書き込み, 読み込み, 古い応答ばかり, 長さが変, 残りのセクタの読み込み) */
static struct { char tag[8]; uint32_t wr, rd, stale, len, rd2; } zerr = { "SLPDZERR" };
static uint32_t zstat_tos;              /* TOS を 0 にしたパケットの数 */
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
extern volatile uint8_t scsi_zown;      /* npd.x の命令が終わって段階 (+1) を 0 に戻すまでの間 1 */
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
  /* 書き込みに失敗したら 1 回だけ書き直す (届いていて二度送ることになっても、TCP が重複を捨てる) */
  if (zwrite(ZWLBA, (16 + txlen + 511) / 512, zreq) != 0) {
    zerr.wr++;
    if (zwrite(ZWLBA, (16 + txlen + 511) / 512, zreq) != 0)
      return -1;
  }
  /* Pico は次の要求が来るまで応答を持っているので、読み込みに失敗しても同じ応答を読み直せる。
     以前は 1 回の失敗で交換ごと捨てていて、送ったパケットと受け取るはずのパケットが失われ、TCP の再送を待った */
  for (try = 0; ; try++) {
    if (zread(zstep(8), first, zres) != 0) {
      zerr.rd++;
      if (try >= 64)
        return -1;
      continue;
    }
    if (r->magic == q->magic && r->seq == zseq)
      break;
    zstat.retry++;
    if (try >= 64) {
      zerr.stale++;
      return -1;
    }
    /* キャッシュに残っていた古い応答: 読み出し領域のどこか別の場所 (番号と回数から作る疑似乱数) に飛んで読み直す。
       リセットを繰り返すと、前の回までに読んだ範囲 (100MB を超えることもある) がキャッシュに残っている。
       以前は 16MB ずつ飛んでいて、480MB の領域では 30 回で同じ場所に戻るので、64 回試しても 30 か所しか見ず、
       それが全部キャッシュ済みだと抜け出せなかった。キャッシュに当たる読み込みは USB を通らないので安い */
    {
      uint32_t h = (zseq ^ ((uint32_t)try << 20)) * 2654435761u;
      zsect = ZRLBA + 8 + (h >> 8) % (ZRSPAN / 8 - 1) * 8;   /* 次の zstep(8) で 8 下がる */
    }
  }
  n = r->len;
  if (n > ZDATA) {
    zerr.len++;
    return -1;
  }
  /* 残りのセクタの読み込みに失敗したら、同じ位置をもう一度読む (場所を変えると別の応答になりうる) */
  if (16 + n > first * 512) {
    int k;
    for (k = 0; zread(zsect + first, (16 + n - 1) / 512 + 1 - first, zres + first * 512) != 0; k++) {
      zerr.rd2++;
      if (k >= 3)
        return -1;
    }
  }
  zstat.xchg++;
  zstat.drops = r->stat;
  *more = r->more;
  return n;
}

/* Pico から来たレコードを 1 個ずつ xip.x に渡す。空き 14 バイトに npd.sys が Ethernet の見出しを書くので、写さずに済む */
static void rec_in(uint8_t *s, int n)
{
  while (n >= 16) {
    int len = (s[0] << 8) | s[1];
    int rl = 16 + ((len + 1) & ~1);
    uint8_t *ip = s + 16;
    if (rl > n)
      break;
    if (len >= TSP_HDR && ip[0] == TSP_MARK) {
      /* tis.x のソケット応答 */
      long arg[2];
      arg[0] = len;
      arg[1] = (long)ip;
      tap('S', ip, len);
      /* 入口は取り直す: 交換の SCSI を待つ間 (_CHANGE_PR) に tis.x -r で外されて解放されたかもしれない。
         このスレッドはスーパーバイザで動くので、取り直してから呼ぶまでの間には切り替わらない */
      tis = (void *)call_etd(etd, 26, 0);
      if (tis == (void *)-1)
        tis = 0;
      if (tis && (call_etd(tis, TIS_NP_IN, arg) & 1))
        tsp_more = 1;
    } else if (len >= ZSS_HDR && ip[0] == ZSS_MARK) {
      /* zos.x の端末のセッションの応答 (入口の取り直しは TSP と同じ理由) */
      long arg[2];
      arg[0] = len;
      arg[1] = (long)ip;
      tap('Z', ip, len);
      zss = zss_check((void *)call_etd(etd, NPD_ETD_ZSS_GET, 0));
      if (zss && (call_etd(zss, ZSS_NP_IN, arg) & 1))
        tsp_more = 1;
    } else if (len >= ACCEL_RES_HDR && ip[0] == ACCEL_MARK) {
      /* 暗号アクセラレータの応答 (IPv4 のバージョン nibble と重ならない) */
      long arg[2];
      arg[0] = len;
      arg[1] = (long)ip;
      tap('A', ip, len);
      call_etd(etd, 23, arg);
    } else if (len >= 20 && (ip[0] >> 4) == 4 && (ip[0] & 15) >= 5 && ((ip[2] << 8) | ip[3]) == len) {
      long arg[2];
      if (ip[1]) {
        /* xip.x は TOS (DSCP) が 0 でない TCP のデータに RST を返して捨てる (KA9Q NOS 由来)。OpenSSH の sshd は
           IPQoS でデータに TOS 0xB8 を付けるので、接続はできても最初のデータで切られた。渡す前に 0 にし、
           IP ヘッダのチェックサムを差分で直す (RFC 1624。TCP のチェックサムに TOS は入らない) */
        uint32_t c = (uint16_t)~((ip[10] << 8) | ip[11]);
        c += (uint16_t)~((ip[0] << 8) | ip[1]);
        c += ip[0] << 8;
        c = (c & 0xffff) + (c >> 16);
        c = (c & 0xffff) + (c >> 16);
        c = (uint16_t)~c;
        ip[1] = 0;
        ip[10] = c >> 8;
        ip[11] = c;
        zstat_tos++;
      }
      arg[0] = len;
      arg[1] = (long)(s + 2);
      tap('R', ip, len);
      if (call_etd(etd, 16, arg) > 0)
        stat.rx++;
    } else {
      tap('B', ip, len);
      stat.bad++;
    }
    s += rl;
    n -= rl;
  }
}

static int ticks(void)
{
  return _iocs_ontime().sec;            /* 1/100 秒 (0 時に戻る) */
}

static int ticks(void);

/* -m: zmux の線のチャネル 1 で 1 回交換する。要求のブロック (見出し 16 + レコード) をそのまま送り、応答のブロックを待つ。
   戻り値は zxchg と同じ (受信データ長、失敗なら -1) */
static int mxchg(int txlen, int *more)
{
  struct zhdr *q = (struct zhdr *)zreq, *r = (struct zhdr *)zres;
  long a[2];
  int t0 = ticks(), n;
  q->magic = ZMAGIC;
  q->seq = ++zseq;
  q->len = txlen;
  q->more = ZDATA;
  q->stat = zmyip;
  a[0] = (long)zreq;
  a[1] = 16 + txlen;
  while (call_etd(mux, 2, a) < 0) {      /* 線が UP でない / 前の要求を送りかけ */
    if ((unsigned)(ticks() - t0) > 1000) { zerr.wr++; return -1; }
    _dos_change_pr();
  }
  a[0] = (long)zres;
  a[1] = sizeof zres;
  for (;;) {
    n = call_etd(mux, 3, a);
    if (n > 0)
      break;
    /* 19200bps では 4KB の要求と 4KB の応答で 4 秒余りかかる */
    if (n < 0 || (unsigned)(ticks() - t0) > 1500) { zerr.rd++; return -1; }
    _dos_change_pr();
  }
  if (n < 16 || r->magic != q->magic || r->seq != zseq || r->len > ZDATA || 16 + r->len > n) {
    zerr.len++;
    return -1;
  }
  zstat.xchg++;
  zstat.drops = r->stat;
  *more = r->more;
  return r->len;
}

static void zthread(void)
{
  int last = 0, hot = 0, act = 0, zfails = 0;
  for (;;) {
    int n, nip, now, r, more = 0, turg = 0, zurg = 0;
    stat.loops++;
    {
      long arg[2];
      arg[0] = (long)(zreq + 16);
      arg[1] = ZDATA;
      n = call_etd(etd, 19, arg);
      if (n > 2)
        tap('T', zreq + 18, n - 2);
    }
    nip = n;
    tis = (void *)call_etd(etd, 26, 0);
    if (tis == (void *)-1)               /* npd.sys 1.1 (ETD 26 が無い) */
      tis = 0;
    if (tis) {
      long a[3];
      a[0] = 0;
      a[1] = 0;                          /* 問い合わせだけ: tis.x に急ぎ (送るもの) があるか */
      a[2] = 0;
      call_etd(tis, TIS_NP_OUT, a);
      turg = a[2];
    }
    zss = zss_check((void *)call_etd(etd, NPD_ETD_ZSS_GET, 0));   /* npd.sys 1.2 までは -1 (ETD 28 が無い) */
    zurg = 0;
    if (zss) {
      long a[3];
      a[0] = 0;
      a[1] = 0;
      a[2] = 0;
      call_etd(zss, ZSS_NP_OUT, a);
      zurg = a[2];
    }
    now = ticks();
    /* -m: 線を空回りさせない。送るものも急ぎも無ければ、Pi の KICK か 2 秒に 1 回だけ交換する */
    if (mux && n == 0 && hot == 0 && !turg && !zurg && !call_etd(mux, 4, 0) && (unsigned)(now - last) < 200) {
      _dos_change_pr();
      continue;
    }
    if (!mux && n == 0 && hot == 0 && !turg && !zurg && (unsigned)(now - last) < ((unsigned)(now - act) < 200 ? zidle : ZIDLE2)) {
      _dos_change_pr();
      continue;
    }
    last = now;
    if (tis && n + 2 + TSP_HDR <= ZDATA) {
      /* 0xA7 も載せるときは場所を分ける。Pico は 0xA7 の要求が来た交換でだけ 0xA7 に答えるので、0xA7 には少なくとも見出しの
         分を残す。どちらも急ぐときは交互に: TSP に (見出しの分を除いて) 全部 / 半分ずつ。tis.x も zos.x も、答えの来ない要求は
         同じ長さで送り直すので、半分にし続けると大きい要求が二度と載らない (tis.x は 3 秒で TSP をあきらめる)。
         0xA7 の新しい要求は半分までにするので (下)、0xA7 の送り直しは半分ずつの交換で必ず載る */
      long tmax = ZDATA - n - 2;
      if (zss) {
        tmax -= 2 + ZSS_HDR + 2;
        if (zurg && turg) {
          if (!share_tsp) tmax = (ZDATA - n) / 2 - 2;
          share_tsp = !share_tsp;
        } else if (zurg) tmax = (ZDATA - n) / 2 - 2;
      }
      /* TSP レコード [長さ 2][要求][詰め物] を IP / アクセラレータのレコードの後ろに。tis.x は交換のたびに 1 個
         (送るものが無くても見出しだけ) 返す。前の交換の応答が来ていなければ同じ要求をもう一度 */
      long a[3], m;
      a[0] = (long)(zreq + 16 + n + 2);
      a[1] = tmax & ~1L;
      a[2] = 0;
      m = tmax >= TSP_HDR ? call_etd(tis, TIS_NP_OUT, a) : 0;
      if (m > 0) {
        zreq[16 + n] = m >> 8;
        zreq[16 + n + 1] = m;
        if (m & 1)
          zreq[16 + n + 2 + m] = 0;
        n += 2 + ((m + 1) & ~1);
      }
    }
    if (zss && n + 2 + ZSS_HDR <= ZDATA) {
      /* 0xA7 のレコードも同じ形で後ろに (zos.x は交換のたびに 1 個、送るものが無くても見出しだけ返す)。新しい要求は ZDATA の
         半分まで (送り直しが半分ずつの交換で必ず載るように) */
      long a[3], m;
      a[0] = (long)(zreq + 16 + n + 2);
      a[1] = ZDATA - n - 2;
      if (a[1] > ZSS_CAP) a[1] = ZSS_CAP;
      a[2] = 0;
      m = call_etd(zss, ZSS_NP_OUT, a);
      if (m > 0) {
        zreq[16 + n] = m >> 8;
        zreq[16 + n + 1] = m;
        if (m & 1)
          zreq[16 + n + 2 + m] = 0;
        n += 2 + ((m + 1) & ~1);
      }
    }
    tsp_more = 0;
    r = mux ? mxchg(n, &more) : zxchg(n, &more, (unsigned)(now - act) < 20 ? ZSECTS : 1);
    if (r < 0) {
      /* 送るはずだったレコードは失われる (TCP が再送する)。続けて失敗したときだけ 1 秒待つ
         (Pico が居ない・つながらない、"X6NR" を知らない古いファーム) */
      zstat.err++;
      if (++zfails >= 2)
        while ((unsigned)(ticks() - now) < 100)
          _dos_change_pr();
      continue;
    }
    zfails = 0;
    stat.tx += n;
    {
      uint32_t rx0 = stat.rx;
      rec_in(zres + 16, r);
      if (nip || turg || tsp_more || stat.rx != rx0) {
        int k = ztrace.pos++ % ZTRACE;
        act = now;
        ztrace.e[k].t = now;
        ztrace.e[k].tx = n;
        ztrace.e[k].rx = r;
        ztrace.e[k].pk = stat.rx - rx0;
      }
    }
    /* Pico に続きがあるときだけ間を置かずに問い合わせる。送るものができたら (TCP の ACK など) その場で交換する。
       空の交換 (SCSI の読み書き) を続けると CPU を食って xip.x の番が回らない */
    hot = more || tsp_more ? 1 : 0;
    _dos_change_pr();
  }
}

int npd_main(void)
{
  int ssp;
  {
    unsigned char *p = cmd_line + 1;     /* 先頭 1 バイトは長さ */
    for (; *p; p++)
      if (p[0] == '-' && (p[1] | 0x20) == 'm') {
        mux = (void *)1;                 /* 下で zmux.sys を探す */
        p++;
      } else if (p[0] == '-' && (p[1] | 0x20) == 'i' && p[2] >= '0' && p[2] <= '9') {
        /* -i N: やり取りの直後の問い合わせ間隔 (1/100 秒)。短いほど応答は速いが、空の交換が CPU を食う */
        zidle = 0;
        for (p += 2; *p >= '0' && *p <= '9'; p++)
          zidle = zidle * 10 + (*p - '0');
        p--;
      } else if (*p >= '0' && *p <= '9') {
        /* a.b.c.d: Z の IP アドレス */
        uint32_t ip = 0, v = 0;
        int dots = 0, digits = 0;
        for (; (*p >= '0' && *p <= '9') || *p == '.'; p++) {
          if (*p == '.') {
            ip = (ip << 8) | v;
            v = digits = 0;
            dots++;
          } else {
            v = v * 10 + (*p - '0');
            digits++;
          }
        }
        p--;
        if (dots == 3 && digits && v < 256)
          zmyip = (ip << 8) | v;
      }
  }
  if (!zmyip) {
    _dos_print("usage: npd.x [-m] [-iN] a.b.c.d   (a.b.c.d = Z の IP アドレス、-m = zmux の線で)\r\n");
    return 1;
  }
  ssp = _dos_super(0);
  etd = find_npd();
  if (mux && !(mux = find_zmux())) {
    if (ssp > 0) _dos_super(ssp);
    _dos_print("npd: zmux.sys が組み込まれていません\r\n");
    return 1;
  }
  if (!etd) {
    if (ssp > 0) _dos_super(ssp);
    _dos_print("npd: npd.sys (/dev/ep0) が組み込まれていません\r\n");
    return 1;
  }
  if (call_etd(etd, 18, 0)) {          /* スレッドは常駐済み (受け入れの再開だけ) */
    if (ssp > 0) _dos_super(ssp);
    _dos_print("npd: 受信を再開しました (常駐済み)\r\n");
    return 0;
  }
  {
    /* 番号と読み始める位置を毎回変える。Z 側の読み込みキャッシュは X68000 をリセットしても残るので、
       1 から数え直すと、前の回の応答 (番号も合う) を読んでしまい、古いパケットが流れ込んだ */
    uint32_t h = ((uint32_t)_dos_getdate() << 17) ^ (uint32_t)_dos_gettim2() ^ ((uint32_t)_iocs_ontime().sec << 7);
    h *= 2654435761u;
    zseq = h;
    zsect = ZRLBA + (h >> 8) % (ZRSPAN / 8) * 8;
  }
  if (!mux) {
    /* SCSI の入口にフックを挟む (zio を参照)。スレッドを作る前に、スーパーバイザで */
    extern void scsi_hook(void);
    extern uint32_t scsi_old;
    volatile uint32_t *vec = (volatile uint32_t *)(0x400 + 0xf5 * 4);
    scsi_old = *vec;
    *vec = (uint32_t)scsi_hook;
  }
  if (ssp > 0) _dos_super(ssp);
  if (!mux) {
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
    int tid = _dos_open_pr("npd", 2, (int)(ustack + sizeof ustack), (int)((char *)sstack + sizeof sstack),
                           0x2000, (int)zthread, &prc, 0);
    static char msg[4];
    static struct dos_prcptr me;        /* _GET_PR は情報をここに書く (NULL を渡すとアドレス 0 を壊す) */
    int myid, r;
    if (tid < 0) {
      _dos_print("npd: スレッドを作れません (CONFIG.SYS の PROCESS を確認)\r\n");
      return 1;
    }
    myid = _dos_get_pr(-1, &me);
    while ((r = _dos_send_pr(myid, tid, 0, msg, sizeof msg)) == -28)   /* 使用中なら譲ってから送り直す (xip.x と同じ) */
      _dos_change_pr();
    if (r < 0)
      _dos_print("npd: スレッドを起こせません\r\n");
  }
  _dos_print(mux ? "npd: netpipe thread started (zmux line, channel 1)\r\n" : "npd: netpipe thread started (Pico W, SCSI ID 6, bridge)\r\n");
  {
    /* 常駐する大きさは BSS (スレッドのスタック) まで含める */
    extern char _end;
    _dos_keeppr(&_end - (mem_ptr + 0x100), 0);
  }
  return 0;
}
