/* ホスト (Mac) で tis.c / lo.c をビルドするための x68k/dos.h の代わり (tests/hostsim.c が中身を持つ) */
#ifndef HOST_X68K_DOS_H
#define HOST_X68K_DOS_H
int _dos_super(int stack);
void _dos_change_pr(void);
#endif
