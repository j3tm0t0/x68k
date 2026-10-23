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
#include "../tsp_lwipopts.h"
#endif
