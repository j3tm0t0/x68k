#ifndef _X68K_NETINET_IP_H_
#define _X68K_NETINET_IP_H_
/* IPTOS 定数のみ (dropbear の set_sock_priority 用)。xip は TOS!=0 を嫌うので
   実際にはセットしても効かない/避けるが、コンパイルを通すために定義する。 */
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_MINCOST 0x02
#ifndef IPTOS_DSCP_AF21
#define IPTOS_DSCP_AF21 0x48
#endif
#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xb8
#endif
#define IPTOS_CLASS_CS1 0x20
#endif
