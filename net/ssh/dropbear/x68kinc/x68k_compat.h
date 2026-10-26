#ifndef _X68K_COMPAT_H_
#define _X68K_COMPAT_H_
/* libsocket のソケット fd は 128..159。fd_set が収まるよう FD_SETSIZE を広げる
   (sys/select.h は #ifndef ガードなので、全 TU の先頭で先に定義する) */
#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#endif
/* elf2x68k の socket ヘッダに無い定数を補う (全 TU に -include で先頭挿入)。
   libsocket の getaddrinfo は ai_family を無視し常に AF_INET を返すので、
   AF_UNSPEC の値は AF_INET(0) と異なればよい。 */
#ifndef AF_UNSPEC
#define AF_UNSPEC 0xff
#endif
#ifndef PF_UNSPEC
#define PF_UNSPEC AF_UNSPEC
#endif

#ifndef AF_MAX
#define AF_MAX 0x100
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef IPPROTO_IP
#define IPPROTO_IP 0
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_LINGER
#define SO_LINGER 0x80
#endif
#ifndef IP_TOS
#define IP_TOS 3
#endif
struct x68k_linger { int l_onoff; int l_linger; };
#ifndef __x68k_linger_defined
#define __x68k_linger_defined
#define linger x68k_linger
#endif

/* libsocket のソケット fd は 128..159。それ未満は Human68k の通常ファイル/コンソール */
#define X68K_SOCKFD_MIN 128

/* 位相ごとのデバッグ印 (x68k_shim.c)。-DX68K_SSH_DEBUG のビルドでのみ出力する */
void x68dbg(const char *m);
const char *x68k_ssh_dir(const char *homedir, int create);   /* known_hosts と鍵の置き場 (x68k_shim.c) */

/* パスワード / keyboard-interactive の入力 (x68k_pass.c)。echo=0 ならエコー無し。ESC / Ctrl-C で NULL */
char *x68k_readpass(const char *prompt, int echo);

#endif
