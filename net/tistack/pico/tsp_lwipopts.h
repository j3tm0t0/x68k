/* tsp_svc (ソケット窓口) が前提にする lwIP の大きさ (Pico 2 W に置くときの見込み、README の RAM の見積り)。
   ファームの lwipopts.h から include する。ホストの試験 (pico/host/lwipopts.h) は先に別の値を決めてよい */
#ifndef TSP_LWIPOPTS_H
#define TSP_LWIPOPTS_H
#define TCP_MSS                 1460
#define TCP_WND                 (4 * TCP_MSS)       /* ソケットあたり Pico が持つ受信の上限 (Z に渡すまで) */
#define TCP_SND_BUF             (8 * TCP_MSS)       /* 1 交換分 (約 4KB) を書いた直後でも次の交換に CREDIT が残る */
#define TCP_SND_QUEUELEN        24
#ifndef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG        48
#endif
#ifndef MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB        40
#endif
#ifndef MEMP_NUM_TCP_PCB_LISTEN
#define MEMP_NUM_TCP_PCB_LISTEN 8
#endif
#ifndef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB        12
#endif
#ifndef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE          16
#endif
#ifndef MEM_SIZE
#define MEM_SIZE                (20 * 1024)         /* 送信の pbuf (TSP_TXPOOL 12KB + 見出し・制御パケット) */
#endif
#define MEM_ALIGNMENT           4
#define TCP_LISTEN_BACKLOG      0                   /* backlog は tsp_svc が数える */
#define TCP_QUEUE_OOSEQ         1
#define LWIP_TCP_KEEPALIVE      1
#define SO_REUSE                1
#endif
