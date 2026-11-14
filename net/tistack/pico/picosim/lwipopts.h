/* harness/picosim (XEiJ の試験台) に tsp_svc を載せるときの lwIP の設定: NO_SYS、Ethernet (slirp の LAN)、
   大きさは Pico 2 W に置くときと同じ (../tsp_lwipopts.h) */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_DNS                1
#define DNS_LOCAL_HOSTLIST      1
#define DNS_LOCAL_HOSTLIST_IS_DYNAMIC 1
#define LWIP_STATS              0
#define LWIP_DEBUG              0
#ifdef TSP_BS
/* BlueSCSI v2 (Pico 2 W) の値 (make BS=1、../host/lwipopts.h と同じ) */
#define TCP_MSS                 1460
#define TCP_WND                 (22 * TCP_MSS)
#define TCP_SND_BUF             (4 * TCP_MSS)
#define TCP_SND_QUEUELEN        ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
/* lwIP の表もファーム (BlueSCSI-v2 の src/x68stage_lwip/lwipopts.h) と同じ大きさ */
#define PBUF_POOL_SIZE          32
#define MEM_SIZE                (8000 + 2048)
#define MEMP_NUM_TCP_PCB        (4 + 8)
#define MEMP_NUM_TCP_PCB_LISTEN (1 + 4)
#define MEMP_NUM_UDP_PCB        (4 + 4)
#define MEMP_NUM_TCP_SEG        (32 + 8)
#define DNS_TABLE_SIZE          2
#define DNS_MAX_NAME_LENGTH     64
#endif
#include "../tsp_lwipopts.h"
#endif
