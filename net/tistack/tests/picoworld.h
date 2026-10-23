/*
 * picoworld.h -- picosim の「Pico と LAN」の側: 本物の lwIP (NO_SYS、仮想の時計) + tsp_svc + LAN の相手
 *
 * LAN は lwIP の中のループバック: Pico の IP 10.0.0.1 に相手のサーバ (echo / discard / source / sshd / UDP echo /
 * DNS) が居て、Z からの接続は Pico (tsp_svc) が lwIP で 10.0.0.1 につなぐ。10.0.0.0/24 のほかの番地は
 * 黒い穴 (SYN が返らない = 時間切れ)、それ以外は経路が無い (届かない)。
 * tis.c と名前がぶつからないよう、lwIP はこちらの翻訳単位にだけ置く。
 */
#ifndef PICOWORLD_H
#define PICOWORLD_H
#include <stdint.h>

#define PICO_IP     0x0a000001          /* 10.0.0.1 */
#define HOLE_IP     0x0a000063          /* 10.0.0.99: 黒い穴 */
#define NOROUTE_IP  0xc0a84d01          /* 192.168.77.1: 経路なし */

extern uint64_t world_us;               /* 仮想の時計 (µs) */

void world_init(uint32_t pepoch);
void world_poll(void);
int world_exchange(const uint8_t *req, int n, uint8_t *res, int max, int *more);
void world_pico_reboot(uint32_t pepoch);
int world_pico_sockets(void);
void world_stat(void);                  /* tsp_stat と lwIP の使用量を表示 */
uint32_t world_errmem(void), world_held_peak(void), world_sndq_peak(void), world_dup(void);
int world_tcp_pcbs(void);               /* lwIP の使用中の TCP pcb (listen を除く) */

/* 中身の型 (どちらの向きも同じ): 位置 k のバイト */
static inline uint8_t pat(uint32_t k) { return (uint8_t)(k * 7 + (k >> 8) + (k >> 16)); }

/* discard (9): 受けたバイト数・型違い・FIN を受けた接続の数 */
struct discard_stat { uint64_t bytes; uint32_t bad, fins, conns; uint64_t done_us; };
extern struct discard_stat dstat;
/* source (19): "N\n" を受けたら N バイトの型を送って閉じる */
extern uint32_t src_conns;
/* echo (7): 受けた接続の数、RST を受けた数 */
extern uint32_t echo_conns, echo_resets;
/* reset (70): 何か受けたら RST で切る */
/* sshd (22): バナー + KEXINIT を送り、相手のバナー + KEXINIT を受けたら閉じる */
extern uint32_t sshd_got;

/* LAN 側のクライアント (Z の listen につなぐ)。番号を返す */
#define NCLIENT 8
struct client {
  int used, connected, refused, closed, reset;
  uint8_t rx[65536];
  uint32_t rxlen;
  uint32_t lport;
};
extern struct client cl[NCLIENT];
int client_start(uint16_t port, const void *data, uint32_t n, int close_after);
void client_send(int k, const void *data, uint32_t n);
void client_close(int k);
void udp_peer_send(uint16_t from_port, uint16_t to_port, const void *data, uint32_t n);
extern uint32_t udp_peer_got;           /* UDP 9999 (相手側の受け口) で受けた数 */

#endif
