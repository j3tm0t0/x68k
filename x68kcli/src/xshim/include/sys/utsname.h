/* newlib に無い <sys/utsname.h> (実装は x68shim.c の uname) */
#ifndef _XSHIM_SYS_UTSNAME_H
#define _XSHIM_SYS_UTSNAME_H
#define _UTSNAME_LENGTH 32
struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};
int uname(struct utsname *);
#endif
