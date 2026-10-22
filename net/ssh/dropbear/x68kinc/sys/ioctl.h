#ifndef _X68K_SYS_IOCTL_H_
#define _X68K_SYS_IOCTL_H_
/* Human68k/elf2x68k には sys/ioctl.h が無い。dropbear 用の最小スタブ。
   実機では tty の ioctl は使わない (端末サイズは vt100 側 vt_get_size で渡す)。 */
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif
int ioctl(int fd, unsigned long request, ...);
#endif
