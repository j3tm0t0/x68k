/* elf2x68k (newlib) に無い POSIX / MS-DOS 系関数の最小実装 (sed / grep 用) */
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
mode_t umask(mode_t m) { (void)m; return 022; }
int chmod(const char *p, mode_t m) { (void)p; (void)m; return 0; }   /* Human68k の属性は使わない */
int _fileno(FILE *f) { return fileno(f); }
int _setmode(int fd, int mode) { (void)fd; (void)mode; return 0; }   /* 常にバイナリ扱い */
#include <errno.h>
#include <unistd.h>
int access(const char *p, int m) { struct stat st; (void)m; return stat(p, &st); }   /* 存在確認だけ */
long sysconf(int name) { (void)name; return 8192; }   /* grep はページサイズ (_SC_PAGESIZE) に使う */
