#!/usr/bin/env python3
"""GNU のソース (gnulib 入り) を elf2x68k (newlib + libx68k) で組めるように直す。
usage: gnulib_x68k.py pre|post SRCDIR
  pre:  configure の前 (configure と *.in.h)
  post: configure の後 (生成された lib/*.h と gnulib の未移植部分)
直す先が無いファイルは黙って飛ばす (sed / grep / coreutils / findutils / diffutils / gzip で共通に使う)。
"""
import os, sys

stage, d = sys.argv[1], sys.argv[2]


def ed(p, old, new, count=1, guard=None):
    # guard: 直したあとにだけ現れる文字列 (元の文字列が直したあとにも残る場合に、2 度当てないため)
    p = os.path.join(d, p)
    if not os.path.exists(p):
        return
    s = open(p, encoding='utf-8', errors='surrogateescape').read()
    if old in s and not (guard and guard in s):
        open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace(old, new, count))


if stage == 'pre':
    # newlib の <sys/types.h> は <sys/select.h> を読む。gnulib の sys/select.h がそこから <signal.h> を読むと
    # pthread_attr_t がまだ無くて壊れる (Cygwin と同じ形なので Cygwin と同じ扱いにする)
    ed('lib/sys_select.in.h', 'defined __CYGWIN__', '(defined __CYGWIN__ || defined __NEWLIB__)', count=-1)
    # mountlist (df): ドライブの一覧は自前で作るので、configure には「ある」と答えさせる
    ed('configure', '      mingw* | windows*) ac_list_mounted_fs=found ;;',
       '      mingw* | windows*) ac_list_mounted_fs=found ;;\n      elf*) ac_list_mounted_fs=found ;;')
    sys.exit(0)

# --- post ---
# プログラム名: argv[0] (A:\USR\BIN\SORT.X) から "sort" を作る (src/xshim/x68shim.c の x68_progname)
ed('lib/getprogname.c', '#  error "getprogname module not ported to this OS"',
   '  extern const char *x68_progname (const char *);\n  return x68_progname (0);')
ed('lib/progname.c', '  program_name = argv0;',
   '  {\n    extern const char *x68_progname (const char *);\n    argv0 = x68_progname (argv0);\n  }\n  program_name = argv0;',
   guard='x68_progname')
ed('lib/getdtablesize.c', '  struct rlimit lim;\n\n  if (getrlimit', '#ifndef RLIMIT_NOFILE\n  return 64;\n#else\n  struct rlimit lim;\n\n  if (getrlimit',
   guard='#ifndef RLIMIT_NOFILE')
ed('lib/getdtablesize.c', '    return lim.rlim_cur;\n\n  return INT_MAX;\n}', '    return lim.rlim_cur;\n\n  return INT_MAX;\n#endif\n}',
   guard='INT_MAX;\n#endif')
ed('lib/pipe.c', '# error "This platform lacks a pipe function, and Gnulib doesn\'t provide a replacement. This is a bug in Gnulib."',
   '# include <errno.h>\nint pipe (int fd[2]) { (void) fd; errno = ENOSYS; return -1; }')
ed('lib/getlocalename_l-unsafe.c',
   ' #error "Please port gnulib getlocalename_l-unsafe.c to your platform! Report this to bug-gnulib."',
   '      return (struct string_with_storage) { "C", STORAGE_INDEFINITE };')
# newlib の <sys/utime.h> は time_t を自分で読まない
ed('lib/utime.in.h', '# @INCLUDE_NEXT@ @NEXT_UTIME_H@', '# include <time.h>\n# @INCLUDE_NEXT@ @NEXT_UTIME_H@')
# df のドライブの一覧: A: から Z: のうち、ディスクが入っているもの
ed('lib/mountlist.c', '#include "mountlist.h"', '#include "mountlist.h"\n#ifdef __human68k__\n# include <x68k/dos.h>\n#endif')
ed('lib/mountlist.c', '#if MOUNTED_NOT_PORTED\n# error "Please port gnulib mountlist.c to your platform!"\n#endif', '''#ifdef __human68k__
  {
    int drv;
    for (drv = 1; drv <= 26; drv++)
      {
        char name[4] = { 'A' + drv - 1, ':', 0, 0 };
        /* _DRVCTRL の状態: bit1 = メディアが入っている。無いドライブを _DSKFRE で読むと
           「中止 再実行 無視」の枠が出るので、入っていないものは並べない */
        int st = _dos_drvctrl (0, drv);
        if (st < 0 || !(st & 2))
          continue;
        me = xmalloc (sizeof *me);
        me->me_devname = xstrdup (name);
        name[2] = '/';
        me->me_mountdir = xstrdup (name);
        me->me_mntroot = NULL;
        me->me_type = (char *) "human68k";
        me->me_type_malloced = 0;
        me->me_dummy = 0;
        me->me_remote = 0;
        me->me_dev = drv;
        *mtail = me;
        mtail = &me->me_next;
      }
  }
#elif MOUNTED_NOT_PORTED
# error "Please port gnulib mountlist.c to your platform!"
#endif''')
# grep: 入力と出力が同じファイルかの判定 (Human68k は inode が常に同じなので、いつも「同じ」になる)
ed('src/grep.c', '      && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))', '      && 0 && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))')
# coreutils は 256KB の読み書きの区切り (wc はこれをスタックに取る) を使う。Human68k のスタックは
# 64KB (src/xshim の _stack_size) なので 32KB にする
ed('src/ioblksize.h', 'enum { IO_BUFSIZE = 256 * 1024 };', 'enum { IO_BUFSIZE = 32 * 1024 };')
ed('src/cksum_crc.c', '# define BUFLEN (1 << 16)', '# define BUFLEN (1 << 14)')
# md5sum など: UNIX と同じく、いつもバイナリで読み、印は ' ' (DOS 向けの '*' とテキスト読みをやめる)
ed('src/cksum.c', '      if (O_BINARY && *binary)\n        {\n          if (*binary < 0)\n            *binary = ! isatty (STDIN_FILENO);',
   '      if (O_BINARY)\n        {\n          if (*binary <= 0)\n            *binary = ! isatty (STDIN_FILENO);')
ed('src/cksum.c', 'fp = fopen (filename, O_BINARY ? (*binary ? "rb" : "rt") : "r");', 'fp = fopen (filename, "rb");')
ed('src/cksum.c', '      putchar (binary_file ? \'*\' : \' \');', '      putchar (\' \');')
# fd_reopen: 0〜2 を DOS の _DUP2 で差し替えると親のシェルの標準出力が壊れるので、差し替えずに
# 新しい番号を返し、STDIN_FILENO などを src/xshim の x68_stdfd[] に向ける
ed('lib/fd-reopen.c', '  int fd = open (file, flags, mode);\n',
   '  int fd = open (file, flags, mode);\n#ifdef __human68k__\n  if (0 <= desired_fd && desired_fd <= 2)\n    {\n      if (0 <= fd)\n        x68_stdfd[desired_fd] = fd;\n      return fd;\n    }\n#endif\n')
ed('lib/unistd.in.h', '#endif /* _@GUARD_PREFIX@_UNISTD_H */',
   '#ifdef __human68k__\nextern int x68_stdfd[3];\n# undef STDIN_FILENO\n# undef STDOUT_FILENO\n# undef STDERR_FILENO\n# define STDIN_FILENO (x68_stdfd[0])\n# define STDOUT_FILENO (x68_stdfd[1])\n# define STDERR_FILENO (x68_stdfd[2])\n#endif\n#endif /* _@GUARD_PREFIX@_UNISTD_H */', guard='x68_stdfd')
# 上で STDIN_FILENO などを変数にしたので、case の札に使っているところは数に戻す
for f in ('lib/freopen-safer.c', 'src/sort.c'):
    for n, v in (('STDIN_FILENO', 0), ('STDOUT_FILENO', 1), ('STDERR_FILENO', 2)):
        ed(f, 'case %s:' % n, 'case %d:' % v, count=-1)
# df: "A:/" は gnulib では絶対パスに見えない (\ を区切りにすると Shift_JIS の 2 バイト目の 5C で切れるので、
# filename.h の DOS 向けの扱いは使わない)。ドライブ名で始まるものも通す
ed('src/df.c', '  if (!force_fsu && ! IS_ABSOLUTE_FILE_NAME (mount_point))',
   '  if (!force_fsu && ! IS_ABSOLUTE_FILE_NAME (mount_point)\n      && ! (mount_point[0] && mount_point[1] == \':\'))')
# df FILE: Human68k ではディレクトリを open できないので、開けなければいつも stat で見る
ed('src/df.c', '      if (errno == ENOENT || errno == ENOTDIR)\n        return errno;\n      return stat (file, st) == 0 ? 0 : errno;',
   '      return stat (file, st) == 0 ? 0 : errno;')
