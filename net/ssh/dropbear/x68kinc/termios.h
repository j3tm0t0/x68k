#ifndef _X68K_TERMIOS_H_
#define _X68K_TERMIOS_H_
/* elf2x68k の termios.h は sys/termios.h を要求するが無い。dropbear 用の自己完結スタブ。
   実機では tty の raw 設定は行わない (端末制御は vt100 側)。tcsetattr は no-op。
   tcgetattr は「cooked な端末の既定値」を返す: -t の pty-req で put_termcodes がこの値を
   そのまま sshd に送り、sshd が pty に適用するため。全部 0 だと ECHO/ICANON/ISIG/ONLCR が
   落ちて「エコー無し・階段状の改行・Ctrl-C 不可」の pty になる。 */
#include <sys/types.h>
typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;
#define NCCS 20
struct termios {
  tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
  cc_t c_cc[NCCS];
  speed_t c_ispeed, c_ospeed;
};
/* c_lflag */
#define ECHO 0x0008
#define ECHOE 0x0010
#define ECHOK 0x0020
#define ECHONL 0x0040
#define ICANON 0x0100
#define ISIG 0x0080
#define IEXTEN 0x8000
/* c_iflag */
#define ICRNL 0x0100
#define INLCR 0x0040
#define IXON 0x0400
#define IXOFF 0x1000
#define ISTRIP 0x0020
#define IGNCR 0x0080
#define BRKINT 0x0002
#define INPCK 0x0010
/* c_oflag */
#define OPOST 0x0001
/* c_cflag */
#define CS8 0x0300
#define CSIZE 0x0300
#define PARENB 0x1000
/* c_cc indices */
#define VMIN 5
#define VTIME 6
#define VINTR 0
/* optional actions */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
static inline int tcgetattr(int fd, struct termios *t){
  unsigned i; (void)fd;
  if(!t) return 0;
  for(i=0;i<NCCS;i++) t->c_cc[i]=0;
  t->c_iflag = ICRNL | IXON | BRKINT;
  t->c_oflag = OPOST | 0x0004 /* ONLCR */;
  t->c_cflag = CS8;
  t->c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
  t->c_cc[0 /*VINTR*/]=3;  t->c_cc[1 /*VQUIT*/]=0x1c; t->c_cc[2 /*VERASE*/]=0x7f; t->c_cc[3 /*VKILL*/]=0x15;
  t->c_cc[4 /*VEOF*/]=4;   t->c_cc[VMIN]=1; t->c_cc[VTIME]=0; t->c_cc[8 /*VSTART*/]=0x11;
  t->c_cc[9 /*VSTOP*/]=0x13; t->c_cc[10 /*VSUSP*/]=0x1a;
  t->c_ispeed = t->c_ospeed = 38400;
  return 0;
}
static inline int tcsetattr(int fd, int act, const struct termios *t){ (void)fd;(void)act;(void)t; return 0; }
static inline speed_t cfgetispeed(const struct termios *t){ (void)t; return 38400; }
static inline speed_t cfgetospeed(const struct termios *t){ (void)t; return 38400; }
static inline void cfmakeraw(struct termios *t){ (void)t; }
#endif

/* termcodes.c が無条件に参照する追加の termios 定数 (スタブ)。
   c_cc[] の添字は NCCS(20) 未満で重複しない値、フラグは各語内で適当なビット
   (上の tcgetattr の既定値と同じ番号・ビットを使うこと)。 */
#ifndef VQUIT
#define VQUIT 1
#endif
#ifndef VERASE
#define VERASE 2
#endif
#ifndef VKILL
#define VKILL 3
#endif
#ifndef VEOF
#define VEOF 4
#endif
#ifndef VEOL
#define VEOL 7
#endif
#ifndef VSTART
#define VSTART 8
#endif
#ifndef VSTOP
#define VSTOP 9
#endif
#ifndef VSUSP
#define VSUSP 10
#endif
/* c_iflag */
#ifndef IGNPAR
#define IGNPAR 0x0004
#endif
#ifndef PARMRK
#define PARMRK 0x0008
#endif
#ifndef IXANY
#define IXANY 0x0800
#endif
/* c_oflag */
#ifndef ONLCR
#define ONLCR 0x0004
#endif
/* c_lflag */
#ifndef NOFLSH
#define NOFLSH 0x0200
#endif
#ifndef TOSTOP
#define TOSTOP 0x0400
#endif
/* c_cflag */
#ifndef CS7
#define CS7 0x0200
#endif
#ifndef PARODD
#define PARODD 0x2000
#endif
