#!/usr/bin/env python3
"""GNU のソース (gnulib 入り) を elf2x68k (newlib + libx68k) で組めるように直す。
usage: gnulib_x68k.py pre|post SRCDIR
  pre:  configure の前 (configure と *.in.h)
  post: configure の後 (生成された lib/*.h と gnulib の未移植部分)
直す先が無いファイルは黙って飛ばす (sed / grep / coreutils / findutils / diffutils / gzip で共通に使う)。
"""
import os, sys

stage, d = sys.argv[1], sys.argv[2]


def ed(p, old, new, count=1, guard=None, pkg=None, warn=True):
    # guard: 直したあとにだけ現れる文字列 (元の文字列が直したあとにも残る場合に、2 度当てないため)
    # pkg: そのパッケージ (work/xbuild/<名前>) にだけ当てる (同じ名前のファイルが別のパッケージにもあるとき)
    # warn: 直す先が見つからないときに警告するか
    # gnulib は lib/ のほか gl/lib/ (findutils) に置く包みもあるので両方に当てる
    if pkg and os.path.basename(os.path.normpath(d)) != pkg:
        return
    if p.startswith('lib/'):
        ed1('gl/' + p, old, new, count, guard, warn)
    ed1(p, old, new, count, guard, warn)


def ed1(p, old, new, count=1, guard=None, warn=True):
    p = os.path.join(d, p)
    if not os.path.exists(p):
        return
    s = open(p, encoding='utf-8', errors='surrogateescape').read()
    if old in s and not (guard and guard in s):
        os.chmod(p, os.stat(p).st_mode | 0o200)   # less の tar は読み込み専用で展開される (実行の許可は残す)
        open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace(old, new, count))
    elif warn and not (guard and guard in s) and new not in s:
        # 直す前の文字列も直した後の文字列も無い: 版が上がって直しが当たらなくなったかもしれない
        print('gnulib_x68k.py: 警告: %s に直す先が見つからない: %r' % (os.path.relpath(p, d), old[:60]), file=sys.stderr)


if stage == 'pre':
    # newlib の <sys/types.h> は <sys/select.h> を読む。gnulib の sys/select.h がそこから <signal.h> を読むと
    # pthread_attr_t がまだ無くて壊れる (Cygwin と同じ形なので Cygwin と同じ扱いにする)
    ed('lib/sys_select.in.h', 'defined __CYGWIN__', '(defined __CYGWIN__ || defined __NEWLIB__)', count=-1)
    # mountlist (df): ドライブの一覧は自前で作るので、configure には「ある」と答えさせる
    ed('configure', '      mingw* | windows*) ac_list_mounted_fs=found ;;',
       '      mingw* | windows*) ac_list_mounted_fs=found ;;\n      elf*) ac_list_mounted_fs=found ;;', warn=False)   # mountlist を使わないパッケージには無い
    # vim: configure が -D_FORTIFY_SOURCE=1 を足す (elf2x68k には ssp.h が無い)
    ed('src/auto/configure', '-D_FORTIFY_SOURCE=1', '-D_FORTIFY_SOURCE=0', count=-1)
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
        ed(f, 'case %s:' % n, 'case %d:' % v, count=-1, warn=False)   # sort.c に STDERR_FILENO の札は無い
# df: "A:/" は gnulib では絶対パスに見えない (\ を区切りにすると Shift_JIS の 2 バイト目の 5C で切れるので、
# filename.h の DOS 向けの扱いは使わない)。ドライブ名で始まるものも通す
ed('src/df.c', '  if (!force_fsu && ! IS_ABSOLUTE_FILE_NAME (mount_point))',
   '  if (!force_fsu && ! IS_ABSOLUTE_FILE_NAME (mount_point)\n      && ! (mount_point[0] && mount_point[1] == \':\'))')
# df FILE: Human68k ではディレクトリを open できないので、開けなければいつも stat で見る
ed('src/df.c', '      if (errno == ENOENT || errno == ENOTDIR)\n        return errno;\n      return stat (file, st) == 0 ? 0 : errno;',
   '      return stat (file, st) == 0 ? 0 : errno;')

# --- findutils: fork が無いので、xargs と find -exec はコマンドを _EXEC で子として動かして待つ (src/xshim の x68_spawnvp) ---
ed('xargs/xargs.c', '      wait_for_proc (false, 0u);\n\n      if (pipe (fd))', '''      wait_for_proc (false, 0u);

#ifdef __human68k__
      {
        extern int x68_spawnvp (const char *, char *const []);
        int r = bc_args_exceed_testing_limit (argv) ? (errno = E2BIG, -1) : x68_spawnvp (argv[0], argv);
        if (r < 0)
          {
            if (errno == E2BIG)
              return 0;         /* コマンドラインが 255 バイトを超えた。引数を減らしてやり直してもらう */
            error (0, errno, _("failed to run command %s"),
                   quotearg_n_style (0, locale_quoting_style, argv[0]));
            exit (errno == ENOENT ? XARGS_EXIT_COMMAND_NOT_FOUND : XARGS_EXIT_COMMAND_CANNOT_BE_RUN);
          }
        procs_executed = true;  /* add_proc を通らないので、ここで「動かした」とする (無いと終わりに引数なしでもう 1 回動かす。xargs -n1 の空行) */
        if (r == 255)
          {
            error (0, 0, _("%s: exited with status 255; aborting"),
                   quotearg_n_style (0, locale_quoting_style, argv[0]));
            exit (XARGS_EXIT_CLIENT_EXIT_255);
          }
        if (r != 0)
          child_error = XARGS_EXIT_CLIENT_EXIT_NONZERO;
        return 1;
      }
#endif
      if (pipe (fd))''')
ed('find/exec.c', '  child_pid = fork ();\n  if (child_pid == -1)', '''#ifdef __human68k__
  {
    extern int x68_spawnvp (const char *, char *const []);
    int r = bc_args_exceed_testing_limit (argv) ? (errno = E2BIG, -1) : x68_spawnvp (argv[0], argv);
    if (r < 0)
      {
        error (0, errno, "%s", safely_quote_err_filename (0, argv[0]));
        r = 1;
      }
    execp->last_child_status = r << 8;  /* wait の形 (WEXITSTATUS で r) */
    child_pid = 1;                      /* 下の「子の側」と waitpid を通らないように */
  }
#else
  child_pid = fork ();
#endif
  if (child_pid == -1)''')
W = '''  while (waitpid (child_pid, &(execp->last_child_status), 0) == (pid_t) -1)
    {
      if (errno != EINTR)
        {
          error (0, errno, _("error waiting for the command we executed for %s"),
                 safely_quote_err_filename (0, argv[0]));
          state.exit_status = EXIT_FAILURE;
          return 0;             /* FAIL */
        }
    }
'''
ed('find/exec.c', W, '#ifndef __human68k__\n' + W + '#endif\n', guard='#ifndef __human68k__\n  while (waitpid')
# xargs の大きさの見積り: Human68k の環境は別の領域なのでコマンドラインに数えない (上限は x68_spawnvp が見る 255 バイト)
ed('lib/buildcmd.c', '  size_t len = 0u;\n  char **envp = environ;\n',
   '  size_t len = 0u;\n  char **envp = environ;\n#ifdef __human68k__\n  return 0;\n#endif\n')

# --- gzip: 名前で gunzip / zcat の動きを選ぶので、A:\USR\BIN\GUNZIP.X から "gunzip" を作る ---
ed('gzip.c', '    program_name = gzip_base_name (argv[0]);',
   '#ifdef __human68k__\n    {\n      extern const char *x68_progname (const char *);\n      program_name = (char *) x68_progname (argv[0]);\n    }\n#else\n    program_name = gzip_base_name (argv[0]);\n#endif')
# gzip: gunzip・zcat の名前で動きを変える (GNU_STANDARD=0) のと、標準入出力をバイナリにする
ed('gzip.c', '#ifndef GNU_STANDARD\n# define GNU_STANDARD 1\n#endif', '#ifndef GNU_STANDARD\n# ifdef __human68k__\n#  define GNU_STANDARD 0\n# else\n#  define GNU_STANDARD 1\n# endif\n#endif')
ed('tailor.h', '#ifndef SET_BINARY_MODE\n#  define SET_BINARY_MODE(fd)\n#endif',
   '#if !defined SET_BINARY_MODE && defined __human68k__\n   extern int setmode (int, int);\n#  define SET_BINARY_MODE(fd) setmode(fd, O_BINARY)\n#endif\n#ifndef SET_BINARY_MODE\n#  define SET_BINARY_MODE(fd)\n#endif')

# fnmatch (find -name など): Shift_JIS の 2 バイト目の 5C (\)・5B ([)・5D (]) を、パターンの記号ではなく文字として扱う。
# 1 バイト目 (81-9F, E0-FC) の直後のそれらに \ を足してから本来の fnmatch に渡す (C ロケールのまま)
ed('lib/fnmatch.c', '\nint\nfnmatch (const char *pattern, const char *string, int flags)\n{\n', '''
#ifdef __human68k__
static int fnmatch_sjis (const char *pattern, const char *string, int flags);
int
fnmatch (const char *pattern, const char *string, int flags)
{
  const unsigned char *p;
  size_t extra = 0;
  if (!(flags & FNM_NOESCAPE))
    for (p = (const unsigned char *) pattern; *p; p++)
      if (((*p >= 0x81 && *p <= 0x9f) || (*p >= 0xe0 && *p <= 0xfc)) && p[1])
        {
          if (p[1] == '\\\\' || p[1] == '[' || p[1] == ']')
            extra++;
          p++;
        }
  if (!extra)
    return fnmatch_sjis (pattern, string, flags);
  char *q = malloc (strlen (pattern) + extra + 1), *o = q;
  if (!q)
    return -1;
  for (p = (const unsigned char *) pattern; *p; p++)
    {
      *o++ = *p;
      if (((*p >= 0x81 && *p <= 0x9f) || (*p >= 0xe0 && *p <= 0xfc)) && p[1])
        {
          p++;
          if (*p == '\\\\' || *p == '[' || *p == ']')
            *o++ = '\\\\';
          *o++ = *p;
        }
    }
  *o = 0;
  int r = fnmatch_sjis (q, string, flags);
  free (q);
  return r;
}
# undef fnmatch
# define fnmatch fnmatch_sjis
static
#endif
int
fnmatch (const char *pattern, const char *string, int flags)
{
''', guard='fnmatch_sjis')

# --- patch: newlib に getrlimit が無い。開いたディレクトリの控えの数は決め打ち ---
ed('src/safe.c', 'static rlim_t min_cached_fds = 8;\nstatic rlim_t max_cached_fds;', 'static unsigned long min_cached_fds = 8;\nstatic unsigned long max_cached_fds;')
ed('src/safe.c', '''  struct rlimit nofile;

  if (getrlimit (RLIMIT_NOFILE, &nofile) == 0)
    {
      if (nofile.rlim_cur == RLIM_INFINITY)
        max_cached_fds = RLIM_INFINITY;
      else
	max_cached_fds = MAX (nofile.rlim_cur / 4, min_cached_fds);
    }
  else
    max_cached_fds = min_cached_fds;''', '''#ifdef __human68k__
  max_cached_fds = min_cached_fds;
#else
  struct rlimit nofile;

  if (getrlimit (RLIMIT_NOFILE, &nofile) == 0)
    {
      if (nofile.rlim_cur == RLIM_INFINITY)
        max_cached_fds = RLIM_INFINITY;
      else
	max_cached_fds = MAX (nofile.rlim_cur / 4, min_cached_fds);
    }
  else
    max_cached_fds = min_cached_fds;
#endif''')
ed('src/safe.c', '  if (max_cached_fds != RLIM_INFINITY)', '#ifndef RLIM_INFINITY\n# define RLIM_INFINITY ((unsigned long) -1)\n#endif\n  if (max_cached_fds != RLIM_INFINITY)')

# --- less: Shift_JIS をそのまま通す。LESSCHARDEF で 0x80-0xFF を全部「印字できる字」(1 バイト 1 桁) にすると、
# 全角 (2 バイト) は 2 桁、半角カナ (1 バイト) は 1 桁で、Human68k の画面と合う (latin1 は 0x80-0x9F を制御文字と見る) ---
ed('main.c', '\tno_config = lgetenv("LESSNOCONFIG");', '#ifdef __human68k__\n\tif (getenv("LESSCHARSET") == NULL && getenv("LESSCHARDEF") == NULL)\n\t\tsetenv("LESSCHARDEF", "8bcccbcc18b95.b128.", 0);\n#endif\n\tno_config = lgetenv("LESSNOCONFIG");')
# patch の一時ファイル: 元の名前に ".oXXXXXX" を足すと 8+3 の名前にならない (TwentyOne が無いと作れない)。
# 同じディレクトリに "poXXXXXX" (8 文字、拡張子なし) で作る
ed('src/util.c', '''      for (char const *p = last; *p; p++)
	t += (*t = *p) != '\\n';
      sprintf (t, ".%cXXXXXX", letter);''', '''#ifdef __human68k__
      sprintf (t, "p%cXXXXXX", letter);
#else
      for (char const *p = last; *p; p++)
	t += (*t = *p) != '\\n';
      sprintf (t, ".%cXXXXXX", letter);
#endif''', pkg='patch')

# --- file: der.h の宣言は int、der.c の定義は int32_t。m68k の newlib では int32_t が long なので合わない ---
ed('src/der.h', 'file_protected int der_offs(struct magic_set *, struct magic *, size_t);', 'file_protected int32_t der_offs(struct magic_set *, struct magic *, size_t);')
# file: 型の判定で読む長さの上限が 7MB で、zos.x のペイン (主記憶 2.5MB) では malloc が失敗する。失敗しても libmagic はエラーの文を
# 設定せずに NULL を返し、file.c は printf("ERROR: %s", NULL) で newlib の strlen がバスエラーになっていた (zerr が出す "file: Bus error")。
# 上限は 128KB (型の判定に要るのは先頭の数 KB)。malloc の失敗は file_oomem で文を設定する
ed('src/file.h', '# define FILE_BYTES_MAX (7 * 1024 * 1024)/* how much of the file to look at */',
   '# ifdef __human68k__\n#  define FILE_BYTES_MAX (128 * 1024)\n# else\n#  define FILE_BYTES_MAX (7 * 1024 * 1024)\n# endif')
ed('src/magic.c', '\tif ((buf = CAST(unsigned char *, malloc(ms->bytes_max + SLOP))) == NULL)\n\t\treturn NULL;',
   '\tif ((buf = CAST(unsigned char *, malloc(ms->bytes_max + SLOP))) == NULL) {\n\t\tfile_oomem(ms, ms->bytes_max + SLOP);\n\t\treturn NULL;\n\t}')
# file: MAGIC の区切りは ; (A:/... の : で切れる)。名前は argv[0] (A:\USR\BIN\FILE.X) から "file" を作る
ed('src/file.h', '#if defined(__EMX__) || defined (WIN32)\n#define PATHSEP\t\';\'', '#if defined(__EMX__) || defined (WIN32) || defined(__human68k__)\n#define PATHSEP\t\';\'')
ed('src/file.c', "\tif ((progname = strrchr(argv[0], '/')) != NULL)\n\t\tprogname++;\n\telse\n\t\tprogname = argv[0];",
   "#ifdef __human68k__\n\t{\n\t\textern const char *x68_progname(const char *);\n\t\tprogname = x68_progname(argv[0]);\n\t}\n#else\n\tif ((progname = strrchr(argv[0], '/')) != NULL)\n\t\tprogname++;\n\telse\n\t\tprogname = argv[0];\n#endif")
# patch: 256KB の読み込みの区切りを bss に持ち、起動のたびに 0 で埋めるので 1 回 1.4 秒かかっていた。32KB にする
ed('src/util.h', 'enum { IO_BUFSIZE = 256 * 1024 };', 'enum { IO_BUFSIZE = 32 * 1024 };')

# --- vim: ファイルはバイナリで開く (CR LF を vim が見て fileformat=dos にする)。fork が無いので :! は system() ---
ed('src/vim.h', '#ifdef __CYGWIN__\n# define O_EXTRA    O_BINARY', '#if defined(__CYGWIN__) || defined(__human68k__)\n# define O_EXTRA    O_BINARY')
ed('src/feature.h', '// #define USE_SYSTEM\n', '// #define USE_SYSTEM\n#ifdef __human68k__\n# define USE_SYSTEM\n#endif\n')
ed('src/os_unix.c', '#include "vim.h"\n', '#include "vim.h"\n#ifndef WNOHANG\n# define WNOHANG 1   /* newlib の sys/wait.h に無い (fork が無いので使われない) */\n#endif\n')
# vim の Shift_JIS (encoding=cp932): newlib の mblen は C ロケールで 1 を返すので、先頭バイト (81-9F, E0-FC) を表で決める。
# 「内部のコードは SJIS」の扱い (dbcs_class など) も Windows・Mac と同じにする
ed('src/mbyte.c', '''	else if (enc_dbcs == 0)
	    n = 1;
	else
	{''', '''	else if (enc_dbcs == 0)
	    n = 1;
#ifdef __human68k__
	else if (enc_dbcs == DBCS_JPN)
	    n = ((i >= 0x81 && i <= 0x9f) || (i >= 0xe0 && i <= 0xfc)) ? 2 : 1;
#endif
	else
	{''')
ed('src/mbyte.c', '#if defined(MSWIN) || defined(WIN32UNIX) || defined(MACOS_X)', '#if defined(MSWIN) || defined(WIN32UNIX) || defined(MACOS_X) || defined(__human68k__)', count=-1)

# sort と join は比べる処理が重いので -O2 で組む (XEiJ で sort 4000 行が 8.9 → 7.0 秒、join が 23 → 20 秒)。
# ほかは -Os のまま (-O2 にすると .x が 5〜20% 大きくなり、読み込みが遅くなる)
for f in ('src/sort.c', 'src/join.c'):
    ed(f, '#include <config.h>\n', '#pragma GCC optimize ("O2")\n#include <config.h>\n', guard='#pragma GCC optimize')
# sort -n: 比べるたびに free (NULL) を呼んでいた (gnulib の rpl_free を通る)。数の比べ (strnumcmp) も -O2 に
ed('src/sort.c', '          free (allocated);\n        }\n      else if (ignore)', '          if (allocated)\n            free (allocated);\n        }\n      else if (ignore)', guard='if (allocated)')
ed('lib/strnumcmp.c', '#include <config.h>\n', '#pragma GCC optimize ("O2")\n#include <config.h>\n', guard='#pragma GCC optimize')
# join・uniq など: 欄の区切りを調べる c32issep は 1 文字ごとに iswspace を呼ぶ。ASCII は空白とタブだけなので先に答える
ed('src/system.h', 'c32issep (char32_t wc)\n{\n', 'c32issep (char32_t wc)\n{\n  if (wc < 0x80)\n    return wc == \' \' || wc == \'\\t\';\n', guard='if (wc < 0x80)', pkg='coreutils')
# getc / putc: newlib (m68k-elf) は getc_unlocked・putc_unlocked を関数でしか持たない。1 文字ごとの呼び出しをやめ、newlib のマクロで直に読み書きする
# (__SCLE (CR LF の変換) は Cygwin だけで、ここでは定義されない。CR は shim の read・write で扱っている)
ed('lib/unlocked-io.h', '#endif /* UNLOCKED_IO_H */', '''# if defined __NEWLIB__ && !defined __CUSTOM_FILE_IO__ && !defined __SCLE && defined __sgetc_r
#  undef getc
#  undef getc_unlocked
#  define getc(x) __sgetc_r (_REENT, x)
#  define getc_unlocked(x) __sgetc_r (_REENT, x)
#  undef putc
#  undef putc_unlocked
#  define putc(x, y) __sputc_r (_REENT, x, y)
#  define putc_unlocked(x, y) __sputc_r (_REENT, x, y)
# endif

#endif /* UNLOCKED_IO_H */''', guard='__sgetc_r (_REENT')
# join・uniq: 欄の区切りを探すとき、1 文字ごとに mcel_t を値で渡して述語を関数ポインタで呼んでいた。ASCII はその場で調べる
X68_SKIP = '''newline_or_blank (mcel_t g)
{
  return g.ch == '\\n' || c32issep (g.ch);
}

/* skip_buf_matching (P, LIM, newline_or_blank, OK) と同じ。ASCII は関数を呼ばずに調べる */
static char *
x68_skip_blanks (char *p, char const *lim, bool ok)
{
  while (p < lim)
    {
      unsigned char c = *p;
      if (c < 0x80)
        {
          if ((c == ' ' || c == '\\t' || c == '\\n') != ok)
            break;
          p++;
        }
      else
        {
          mcel_t g = mcel_scan (p, lim);
          if (newline_or_blank (g) != ok)
            break;
          p += g.len;
        }
    }
  return p;
}
'''
for f, v in (('src/join.c', 'ptr'), ('src/uniq.c', 'lp')):
    ed(f, "newline_or_blank (mcel_t g)\n{\n  return g.ch == '\\n' || c32issep (g.ch);\n}\n", X68_SKIP, guard='x68_skip_blanks (char', pkg='coreutils')
    ed(f, 'skip_buf_matching (%s, lim, newline_or_blank, ' % v, 'x68_skip_blanks (%s, lim, ' % v, count=2, pkg='coreutils')
# wc: 1 バイトごとの数え (行・語・行の長さ) が intmax_t (64 ビット) で、68000 では足し算のたびにスタックを読み書きしていた。
# 1 回の read (32KB) の分は long で数えて、読み終わるたびに足す。行の長さ (linepos) は intmax_t のまま持ち、
# この read の中で増えた分だけを long の lp で数える (タブの位置は下位 3 ビットだけで決まる)
WC_OLD = "      bool in_word = false;\n      intmax_t linepos = 0;\n\n      for (ssize_t bytes_read; (bytes_read = read (fd, buf, IO_BUFSIZE)); )\n        {\n          if (bytes_read < 0)\n            {\n              err = errno;\n              break;\n            }\n\n          bytes += bytes_read;\n          char const *p = buf;\n          do\n            {\n              unsigned char c = *p++;\n              switch (c)\n                {\n                case '\\n':\n                  lines++;\n                  FALLTHROUGH;\n                case '\\r':\n                case '\\f':\n                  if (linepos > linelength)\n                    linelength = linepos;\n                  linepos = 0;\n                  in_word = false;\n                  break;\n\n                case '\\t':\n                  linepos += 8 - (linepos % 8);\n                  in_word = false;\n                  break;\n\n                case ' ':\n                  linepos++;\n                  FALLTHROUGH;\n                case '\\v':\n                  in_word = false;\n                  break;\n\n                default:\n                  linepos += wc_isprint[c];\n                  bool in_word2 = !wc_isspace[c];\n                  words += !in_word & in_word2;\n                  in_word = in_word2;\n                  break;\n                }\n            }\n          while (--bytes_read);\n        }\n      if (linepos > linelength)\n        linelength = linepos;\n    }\n"
WC_NEW = "      bool in_word = false;\n      intmax_t linepos = 0;\n\n      for (ssize_t bytes_read; (bytes_read = read (fd, buf, IO_BUFSIZE)); )\n        {\n          if (bytes_read < 0)\n            {\n              err = errno;\n              break;\n            }\n\n          bytes += bytes_read;\n          char const *p = buf;\n          long blines = 0, bwords = 0, lp = 0;   /* この read の分 (lp は linepos に足す分) */\n          do\n            {\n              unsigned char c = *p++;\n              switch (c)\n                {\n                case '\\n':\n                  blines++;\n                  FALLTHROUGH;\n                case '\\r':\n                case '\\f':\n                  if (linepos + lp > linelength)\n                    linelength = linepos + lp;\n                  linepos = 0;\n                  lp = 0;\n                  in_word = false;\n                  break;\n\n                case '\\t':\n                  lp += 8 - (((unsigned long) linepos + lp) & 7);\n                  in_word = false;\n                  break;\n\n                case ' ':\n                  lp++;\n                  FALLTHROUGH;\n                case '\\v':\n                  in_word = false;\n                  break;\n\n                default:\n                  lp += wc_isprint[c];\n                  bool in_word2 = !wc_isspace[c];\n                  bwords += !in_word & in_word2;\n                  in_word = in_word2;\n                  break;\n                }\n            }\n          while (--bytes_read);\n          lines += blines;\n          words += bwords;\n          linepos += lp;\n        }\n      if (linepos > linelength)\n        linelength = linepos;\n    }\n"
ed('src/wc.c', WC_OLD, WC_NEW, pkg='coreutils')
ed('src/wc.c', '#include <config.h>\n', '#pragma GCC optimize ("O2")\n#include <config.h>\n', guard='#pragma GCC optimize', pkg='coreutils')
# sed: 特別な文字を含まない正規表現 (s/abc/.../ や /abc/p) は、regex と dfa を通さず memmem で探す。
# regex は 1 行ごとに状態を作って捨てるので、68000 では 1 行 2 ミリ秒近くかかっていた
ed('sed/sed.h', '  bool begline;\n  bool endline;\n  char re[1];', '  bool begline;\n  bool endline;\n  bool x68_literal;\n  char re[1];', pkg='sed')
ed('sed/regexp.c', '''      if (new_regex->re[0] == '$')
        new_regex->endline = true;
    }
}''', '''      if (new_regex->re[0] == '$')
        new_regex->endline = true;
    }

  /* X68000: 特別な文字の無いものは文字列として探す (match_regex)。大文字小文字を区別しないものと、
     ^ / $ だけのもの、行の区切りの文字を含むものは除く */
  new_regex->x68_literal = new_regex->sz > 0 && !new_regex->begline && !new_regex->endline
                           && !(new_regex->flags & REG_ICASE);
  for (size_t i = 0; new_regex->x68_literal && i < new_regex->sz; i++)
    {
      unsigned char c = new_regex->re[i];
      /* BRE では Shift_JIS の 2 バイト目の ] ^ { | } は regex でも普通の字なので、そのまま文字列として探してよい
         ([ と \\ は regex が特別に扱うので除く。ERE の { | は特別なので、ERE ではこの扱いをしない) */
      if (!(extended_regexp_flags & REG_EXTENDED) && i + 1 < new_regex->sz
          && ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)))
        {
          unsigned char t = new_regex->re[i + 1];
          if (t != '[' && t != '\\\\' && t != '\\n' && t != buffer_delimiter && t != '\\0')
            {
              i++;
              continue;
            }
        }
      if (c == '\\n' || c == buffer_delimiter || c == '\\0' || strchr (".[]\\\\*^$+?(){}|", c))
        new_regex->x68_literal = false;
    }
}''', pkg='sed')
ed('sed/regexp.c', '''  /* Optimized handling for '^' and '$' patterns */
  if (regex->begline || regex->endline)''', '''  if (regex->x68_literal)
    {
      const char *p = memmem (buf + buf_start_offset, buflen - buf_start_offset,
                              regex->re, regex->sz);
      if (!p)
        return 0;
      if (regsize)
        {
          size_t i;

          if (!regarray->start)
            {
              regarray->start = XCALLOC (1, regoff_t);
              regarray->end = XCALLOC (1, regoff_t);
              regarray->num_regs = 1;
            }
          regarray->start[0] = p - buf;
          regarray->end[0] = p - buf + regex->sz;
          for (i = 1; i < regarray->num_regs; ++i)
            regarray->start[i] = regarray->end[i] = -1;
        }
      return 1;
    }

  /* Optimized handling for '^' and '$' patterns */
  if (regex->begline || regex->endline)''', pkg='sed')
# less: ファイルをテキストのまま開くと、newlib (src/xshim の read の包み) が CR を抜くので、lseek で見た長さより読める量が少なく、
# 終わり (EOF) に来ると「まだ続きがある」として読み直し続けて止まっていた (短いファイルと G。CPU 95%、q も効かない)。
# MS-DOS と同じくバイナリで開く (CR LF の CR は less が消して出す)
ed('less.h', '#if MSDOS_COMPILER || OS2\n#define OPEN_READ       (O_RDONLY|O_BINARY)', '#if MSDOS_COMPILER || OS2 || defined(__human68k__)\n#define OPEN_READ       (O_RDONLY|O_BINARY)', pkg='less')
# less: Unicode の「表示しない文字」の表 (omit.uni、U+00AD のソフトハイフン) を、UTF-8 でない (1 バイトずつの) ときにも当てていた。
# Shift_JIS の 2 バイト目が 0xAD の字 (く = 82 AD など) の 2 バイト目が消え、後ろがずれる。UTF-8 のときだけ当てる
ed('charset.c', 'public lbool is_omit_char(LWCHAR ch)\n{\n', 'public lbool is_omit_char(LWCHAR ch)\n{\n\tif (!utf_mode) return FALSE;   /* X68000: Shift_JIS の 2 バイト目の 0xAD を消さない */\n', pkg='less')
# less: 行が画面の幅で折り返すとき、Shift_JIS の 2 バイトの字が右端で割れて、次の行の頭の ESC [ K などと組になって化けていた。
# 1 バイト目が右端の桁に来るときは、その字から次の行に送る (UTF-8 でないときだけ)
ed('line.c', '''public void prewind(lbool contig)
{''', '''#ifdef __human68k__
static int x68_sjis_lead;   /* 直前に置いたのが Shift_JIS の 1 バイト目 */
#endif
public void prewind(lbool contig)
{''', guard='x68_sjis_lead;', pkg='less')
ed('line.c', '''	if (!fits_on_screen(w, a))
		return (1);

	if (rep == NULL)''', '''#ifdef __human68k__
	if (!utf_mode && a != AT_ANSI && w == 1)
	{
		if (x68_sjis_lead)
			x68_sjis_lead = 0;   /* 2 バイト目 */
		else if ((ch >= 0x81 && ch <= 0x9f) || (ch >= 0xe0 && ch <= 0xfc))
		{
			if (!fits_on_screen(2, a))
				return (1);   /* 2 バイトとも入らないので、この字から次の行へ */
			x68_sjis_lead = 1;
		}
	}
#endif
	if (!fits_on_screen(w, a))
		return (1);

	if (rep == NULL)''', guard='2 バイトとも入らないので', pkg='less')
ed('line.c', '''	end_column = 0;
	right_curr = 0;
	right_column = 0;
	cshift = 0;''', '''	end_column = 0;
	right_curr = 0;
	right_column = 0;
	cshift = 0;
#ifdef __human68k__
	x68_sjis_lead = 0;
#endif''', guard='\tx68_sjis_lead = 0;\n#endif', pkg='less')
