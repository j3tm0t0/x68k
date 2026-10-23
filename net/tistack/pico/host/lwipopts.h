/* ホスト (Mac) の試験用 lwIP の設定: NO_SYS、ループバック、仮想の時計 (picosim.c の sys_now) */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_DNS                1
#define DNS_LOCAL_HOSTLIST      1
#define DNS_LOCAL_HOSTLIST_IS_DYNAMIC 1
#define LWIP_HAVE_LOOPIF        1
#define LWIP_ARP                0
#define LWIP_ETHERNET           0
#define LWIP_NETIF_LOOPBACK     1
#define LWIP_LOOPBACK_MAX_PBUFS 0
#define LWIP_STATS              1
#define MEM_STATS               1
#define MEMP_STATS              1
#define LWIP_STATS_DISPLAY      0
#define LWIP_DEBUG              0
/* ループバックは送るたびにヒープへ写すので、ホストではヒープとプールを大きくする
   (Pico が持つ量は tsp_svc の数え (held_peak / sndq_peak) で見る) */
#define MEM_SIZE                (2 * 1024 * 1024)
#define PBUF_POOL_SIZE          64
#define MEMP_NUM_TCP_SEG        256
#define MEMP_NUM_TCP_PCB        128     /* 相手のサーバも同じ lwIP に居る (接続 1 本で pcb 2 個) */
#define MEMP_NUM_TCP_PCB_LISTEN 16
#define MEMP_NUM_UDP_PCB        40
#define MEMP_NUM_PBUF           64
#define MEMP_NUM_SYS_TIMEOUT    16
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#include "../tsp_lwipopts.h"
#endif
