#ifndef _X68K_SYS_UN_H_
#define _X68K_SYS_UN_H_
/* UNIX ドメインソケットは使わない。struct sockaddr_un の最小定義のみ。 */
struct sockaddr_un { short sun_family; char sun_path[108]; };
#endif
