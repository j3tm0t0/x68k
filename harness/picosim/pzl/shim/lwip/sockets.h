/* picosim --pzl のホスト用: PZL の src/net/shim/lwip/sockets.h (libsmb2 のための小さなソケット) の代わり。
   ホストでは SMB につながないので、vd_command.c が使う pzl_poll は失敗を返すだけ */
#ifndef PZL_HOST_LWIP_SOCKETS_H
#define PZL_HOST_LWIP_SOCKETS_H
#include <poll.h>
int pzl_poll(struct pollfd *fds, int nfds, int timeout);
#endif
