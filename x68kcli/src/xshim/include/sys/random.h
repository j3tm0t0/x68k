/* newlib に無い <sys/random.h> (実装は x68shim.c の getrandom。中身は libx68k の getentropy) */
#ifndef _XSHIM_SYS_RANDOM_H
#define _XSHIM_SYS_RANDOM_H
#include <sys/types.h>
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#define GRND_INSECURE 4
ssize_t getrandom(void *, size_t, unsigned int);
#endif
