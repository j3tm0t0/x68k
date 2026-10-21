#ifndef _X68K_TERMIOS_H_
#define _X68K_TERMIOS_H_
/* elf2x68k の termios.h は sys/termios.h を要求するが無い。dropbear 用の自己完結スタブ。
   実機では tty の raw 設定は行わない (端末制御は vt100 側)。tcgetattr/tcsetattr は no-op。 */
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
static inline int tcgetattr(int fd, struct termios *t){ (void)fd; if(t){ unsigned i; char*p=(char*)t; for(i=0;i<sizeof(*t);i++)p[i]=0; } return 0; }
static inline int tcsetattr(int fd, int act, const struct termios *t){ (void)fd;(void)act;(void)t; return 0; }
static inline speed_t cfgetispeed(const struct termios *t){ (void)t; return 0; }
static inline speed_t cfgetospeed(const struct termios *t){ (void)t; return 0; }
static inline void cfmakeraw(struct termios *t){ (void)t; }
#endif

/* termcodes.c が無条件に参照する追加の termios 定数 (スタブ)。
   c_cc[] の添字は NCCS(20) 未満で重複しない値、フラグは各語内で適当なビット。
   pty を要求しないので実値は使われない (コンパイルを通すため)。 */
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
