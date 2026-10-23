/*
 * be.h -- tis.x の API 層 (tis.c) と裏方 (backend: lo.c ループバック、次は Pico) の間の口
 *
 * API 層はソケット表・受信リング・状態・待ちを持つ。backend は「相手との接続を開く・データを運ぶ・閉じる」と
 * 非同期の進行 (tick) を受け持つ。呼ばれるのは常にスーパーバイザモード (排他済み) で、backend は待たない。
 * Pico の backend (pico.c) は 127.0.0.0/8 をループバック (be_loop) に回し、それ以外を npd.x の交換に載せる。
 */
#ifndef TIS_BE_H
#define TIS_BE_H
#include <stdint.h>
#include "tsp.h"

#define NSOCK     32                    /* ソケット番号 128〜159 */
#define SOCKBASE  128
#define MAXQ      8                     /* listen の受け付け待ちの最大 */

/* libsocket (elf2x68k) の socket.c にある struct usock と同じ並び。libsocket は _TI_sock_top で
   この配列の先頭を得て、usock[fd - 128].noblock を直接読み書きする (同じコンパイラで同じ並びになる) */
struct usock {
  int refcnt;
  char noblock;
  char type;
  int rdysock;
  void *p;
  char *name;
  int namelen;
  char *peername;
  int peernamelen;
  char errcodes[4];
  void *obuf;
  void *ibuf;
  char eol[3];
  int flag;
  int flush;
  int event;
};

/* KA9Q の usock.type */
#define TYPE_TCP  1
#define TYPE_UDP  2

/* 基本の状態 (表示用の文字列は tis.c の state_str が送受信の閉じ具合と合わせて作る) */
enum { S_FREE, S_CLOSED, S_LISTEN, S_SYNSENT, S_SYNRCVD, S_EST,
       S_ZOMBIE };                      /* close 済みだが backend が送信を出し切るまで番号を使う (アプリからは無い) */

struct ring {
  uint8_t *buf;
  uint32_t size, head, len;             /* head = 次に読む位置、len = 溜まっている量 */
};

struct tsock {
  uint8_t st;
  uint8_t dgram;                        /* SOCK_DGRAM */
  uint8_t bound;                        /* bind 済み (明示か自動) */
  uint8_t ascii;                        /* SOCK_ASCII */
  uint8_t rx_eof;                       /* 相手が送信を閉じた (FIN を受けた) */
  uint8_t tx_shut;                      /* 自分が送信を閉じた */
  uint8_t rd_shut;                      /* 自分が受信を閉じた */
  uint8_t reset;                        /* 相手のソケットが無くなった */
  uint8_t backlog;                      /* listen の backlog (0 = 自分自身が接続になる、rcmd.x の流儀) */
  uint8_t acc0;                         /* backlog 0 で接続になったがまだ accept されていない */
  uint8_t remote;                       /* Pico 側にソケットがある (pico.c) */
  int8_t peer;                          /* 接続相手の番号 (0〜31)、-1 = 無し */
  int8_t parent;                        /* accept 待ちで並んでいる listen ソケット、-1 = 無し */
  uint8_t qn;
  int8_t q[MAXQ];                       /* accept 待ちの子 */
  uint32_t laddr, raddr;
  uint16_t lport, rport;
  const char *err;                      /* sockerr() が返す文字列 */
  struct ring rx;
};

extern struct usock usock[NSOCK];
extern struct tsock ts[NSOCK];

/* tis.c が backend に貸す道具 */
int sock_alloc(int type);               /* 空きの番号 (0〜31) / -1 */
uint32_t ring_put(struct ring *r, const void *p, uint32_t n);
int is_local(uint32_t addr);
const char *state_str(struct tsock *t);
char *fmt_addr(char *p, uint32_t addr, uint16_t port);   /* "a.b.c.d:port"、終わりを返す */

struct backend {
  const char *name;
  void (*tick)(void);                                     /* 入口と待ちの 1 周ごと */
  int (*listen)(int s);                                   /* 0 / -1 */
  int (*connect)(int s, uint32_t addr, uint16_t port);    /* 0 = 始めた (SYN SENT) / -1 = 拒否・届かない */
  uint32_t (*sendspace)(int s);                           /* いま送れるバイト数 */
  uint32_t (*send)(int s, const void *p, uint32_t n);     /* 受け取ったバイト数 (≦ sendspace) */
  int (*sendto)(int s, uint32_t addr, uint16_t port, const void *p, uint32_t n);  /* UDP。n / -1 */
  void (*shutdown)(int s, int how);                       /* how: 0 受信 / 1 送信 / 2 両方 */
  int (*close)(int s);                                    /* 表から外す前に呼ぶ。1 = 送信を出し切るまで番号を使う */
  /* ここから先は Pico の backend だけ (ループバックは 0) */
  int (*bind)(int s);                                     /* UDP の bind (Pico に受け口を開く) */
  int (*pending)(int s);                                  /* listen / bind の返事待ち: 1 まだ / 0 済んだ / -1 断られた */
  int (*resolve_start)(const char *name);                 /* 名前を引き始める。問い合わせ番号 / -1 */
  int (*resolve_poll)(int q, const char *name, uint32_t *addr);   /* 1 引けた / 0 まだ / -1 引けない */
  int (*np_out)(uint8_t *buf, int max, int *urgent);      /* npd.x: 交換で送る TSP レコード */
  int (*np_in)(const uint8_t *rec, int len);              /* npd.x: 受けた TSP レコード。bit 0 = Pico に続きあり */
};

extern const struct backend be_loop, be_pico;
extern const struct backend *be;

/* tis.x -s / -r が常駐している側から読む (TIS_INFO 命令で先頭を得る) */
#define TIS_INFO  0x7469

struct pico_stat {
  uint32_t xchg, resend, stale_rec, stale, netdown, connects, accepts;
  uint32_t tx_bytes, rx_bytes, dgrams_in, dgrams_out, overrun;
  uint32_t stall;                       /* npd.x が NONPD_CS 来なかった回数 (つながっているソケットは残す) */
};
extern struct pico_stat pstat;
void pico_init(uint8_t *pool, uint32_t txsz, uint32_t seed);
void pico_reset(void);
#define TRACE     128
struct tis_info {
  char tag[8];                          /* "TISINFO" */
  struct usock *usock;
  struct tsock *ts;
  uint32_t nsock, bufsz, own_addr;
  const char *be_name;
  uint32_t calls, waits, conns, rbytes, wbytes;
  uint32_t tpos;                        /* 呼び出しの記録 (tis.x -t で表示)。poll の空振りは数えない */
  struct { uint8_t cmd, fd; uint16_t len; int32_t ret; } trace[TRACE];
  struct pico_stat *pstat;              /* -p のとき (tis.x -s で表示) */
};
extern struct tis_info info;

#endif
