/* newlib に無い <sys/statvfs.h> (実装は x68shim.c の statvfs。DOS の _DSKFRE で空きを数える) */
#ifndef _XSHIM_SYS_STATVFS_H
#define _XSHIM_SYS_STATVFS_H
#include <sys/types.h>
struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    fsfilcnt_t f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
    unsigned long f_type;      /* stat -f が見る (いつも 0x6868 = "hh") */
};
#define ST_RDONLY 1
#define ST_NOSUID 2
int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);
#endif
