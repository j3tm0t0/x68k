/* elf2x68k (newlib) に無い POSIX / MS-DOS 系関数の最小実装 (GNU sed / grep / coreutils / findutils / diffutils 用)
   -ffunction-sections で組み、リンクは --gc-sections で使う分だけ入れる */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <x68k/dos.h>

/* スタック: crt0 の既定は 32KB。GNU の道具には 32KB の区切りをスタックに取るもの (wc) があるので 64KB にする */
long _stack_size = 0x10000;

mode_t umask(mode_t m) { (void)m; return 022; }
int chmod(const char *p, mode_t m) { (void)p; (void)m; return 0; }   /* Human68k の属性は使わない */
int fchmod(int fd, mode_t m) { (void)fd; (void)m; return 0; }
int fchown(int fd, uid_t u, gid_t g) { (void)fd; (void)u; (void)g; return 0; }
int lchown(const char *p, uid_t u, gid_t g) { (void)p; (void)u; (void)g; return 0; }
int _fileno(FILE *f) { return fileno(f); }
/* libx68k の read / write は番号ごとの O_BINARY (__fd_list の flags) を見て、無ければ CR LF と LF を変換する。
   gnulib の set_binary_mode (cat・od・cksum・gzip など) はここに来る */
extern struct { char *name; long flags; } __fd_list[];
int _setmode(int fd, int mode)
{
    int old;
    if (fd < 0 || fd >= 64) { errno = EBADF; return -1; }
    old = (__fd_list[fd].flags & O_BINARY) ? O_BINARY : O_TEXT;
    if (mode == O_BINARY) __fd_list[fd].flags |= O_BINARY;
    else if (mode == O_TEXT) __fd_list[fd].flags &= ~O_BINARY;
    return old;
}
int setmode(int fd, int mode) { return _setmode(fd, mode); }
int access(const char *p, int m) { struct stat st; (void)m; return stat(p, &st); }   /* 存在確認だけ */
long sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX: return 4096;          /* POSIX の最小。本当の上限 (コマンドライン 255 バイト) は x68_spawnvp が E2BIG で知らせる */
    case _SC_CHILD_MAX: return 1;
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return 64;
    case _SC_NPROCESSORS_CONF: case _SC_NPROCESSORS_ONLN: return 1;
    case _SC_PHYS_PAGES: return (12L << 20) / 8192;     /* 主記憶 12MB (sort の既定の作業域の見積りに使う) */
    case _SC_AVPHYS_PAGES: return (4L << 20) / 8192;
    default: return 8192;                   /* _SC_PAGESIZE (grep) など */
    }
}
int fsync(int fd) { (void)fd; return 0; }
int fdatasync(int fd) { (void)fd; return 0; }
void sync(void) { }

/* 利用者は 1 人。名前は環境変数 USER (無ければ user) */
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
int getgroups(int n, gid_t *g) { if (n > 0) g[0] = 0; return 1; }
#include <pwd.h>
#include <grp.h>
static char *username(void) { char *u = getenv("USER"); return u && *u ? u : "user"; }
struct passwd *getpwuid(uid_t u)
{
    static struct passwd pw;
    if (u != 0) return NULL;
    pw.pw_name = username(); pw.pw_uid = 0; pw.pw_gid = 0;
    pw.pw_dir = getenv("HOME") ? getenv("HOME") : "/"; pw.pw_shell = "zosh"; pw.pw_passwd = "";
    pw.pw_gecos = pw.pw_name;
    return &pw;
}
struct passwd *getpwnam(const char *n) { return strcmp(n, username()) == 0 ? getpwuid(0) : NULL; }
struct group *getgrgid(gid_t g)
{
    static struct group gr;
    static char *mem[2];
    if (g != 0) return NULL;
    mem[0] = username(); gr.gr_name = mem[0]; gr.gr_gid = 0; gr.gr_mem = mem; gr.gr_passwd = "";
    return &gr;
}
struct group *getgrnam(const char *n) { return strcmp(n, username()) == 0 ? getgrgid(0) : NULL; }
void endpwent(void) { }
void endgrent(void) { }
void setpwent(void) { }
void setgrent(void) { }
struct passwd *getpwent(void) { return NULL; }
struct group *getgrent(void) { return NULL; }

/* シグナル: Human68k には Break (SIGINT) くらいしかないので signal() に寄せる */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    _sig_func_ptr prev;
    if (act) {
        prev = signal(sig, act->sa_handler);
        if (prev == SIG_ERR) return -1;
    } else {
        prev = signal(sig, SIG_DFL);
        if (prev != SIG_ERR) signal(sig, prev);
    }
    if (old) { memset(old, 0, sizeof *old); old->sa_handler = prev; }
    return 0;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *old) { (void)how; (void)set; if (old) *old = 0; return 0; }
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) { return sigprocmask(how, set, old); }
int sigsuspend(const sigset_t *m) { (void)m; errno = EINTR; return -1; }
unsigned alarm(unsigned s) { (void)s; return 0; }

/* 時刻 */
int clock_gettime(clockid_t c, struct timespec *ts)
{
    struct timeval tv;
    (void)c;
    if (gettimeofday(&tv, NULL) < 0) return -1;
    ts->tv_sec = tv.tv_sec; ts->tv_nsec = tv.tv_usec * 1000;
    return 0;
}
int clock_getres(clockid_t c, struct timespec *ts) { (void)c; if (ts) { ts->tv_sec = 0; ts->tv_nsec = 10000000; } return 0; }
int nanosleep(const struct timespec *rq, struct timespec *rm)
{
    if (rq->tv_sec) sleep(rq->tv_sec);
    if (rq->tv_nsec >= 1000) usleep(rq->tv_nsec / 1000);
    if (rm) rm->tv_sec = rm->tv_nsec = 0;
    return 0;
}

/* ファイルの日時 (touch): DOS の _FILEDATE は開いた番号に対して設定する。newlib に UTIME_NOW が無いので utimensat は置かない */
static int setfdtime(int fd, time_t t)
{
    struct tm *tm = localtime(&t);
    unsigned long dt;
    int r;
    if (!tm) { errno = EINVAL; return -1; }
    dt = ((unsigned long)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday) << 16)
       | (unsigned long)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    r = _dos_filedate(fd, dt);
    if (r < 0 && (unsigned)r >= 0xffff0000u) { errno = EACCES; return -1; }
    return 0;
}
static int setfiletime(const char *path, time_t t)
{
    int fd = _dos_open(path, 1), r;   /* 読み込みで開いた番号には付けられない (-12) */
    if (fd < 0) { errno = ENOENT; return -1; }
    r = setfdtime(fd, t);
    _dos_close(fd);
    return r;
}
/* 開いている番号に付ける (touch は書き込みで開いたまま日時を付けるので、別の番号で付けると閉じたときに今の時刻で上書きされる) */
int futimes(int fd, const struct timeval tv[2]) { return setfdtime(fd, tv ? tv[1].tv_sec : time(NULL)); }
int utimes(const char *path, const struct timeval tv[2]) { return setfiletime(path, tv ? tv[1].tv_sec : time(NULL)); }
#include <utime.h>
int utime(const char *path, const struct utimbuf *u) { return setfiletime(path, u ? u->modtime : time(NULL)); }

/* MS-DOS と同じく、0 バイトの書き込みでその位置から後ろを切り詰める。libx68k の write は 0 バイトだと
   DOS を呼ばないので _WRITE を直に呼ぶ (newlib の番号は DOS の番号と同じ) */
int ftruncate(int fd, off_t len)
{
    if (lseek(fd, len, SEEK_SET) < 0) return -1;
    if (_dos_write(fd, "", 0) < 0) { errno = EIO; return -1; }
    return 0;
}
int truncate(const char *path, off_t len)
{
    int fd = open(path, O_WRONLY), r;
    if (fd < 0) return -1;
    r = ftruncate(fd, len);
    close(fd);
    return r;
}

int pipe(int fd[2]) { (void)fd; errno = ENOSYS; return -1; }
pid_t waitpid(pid_t p, int *st, int o) { (void)p; (void)st; (void)o; errno = ECHILD; return -1; }
pid_t getppid(void) { return 1; }
pid_t setsid(void) { return getpid(); }
int setpgid(pid_t p, pid_t g) { (void)p; (void)g; return 0; }
pid_t getpgrp(void) { return getpid(); }
pid_t tcgetpgrp(int fd) { (void)fd; return getpid(); }
#include <sys/resource.h>
int getrusage(int who, struct rusage *ru) { (void)who; memset(ru, 0, sizeof *ru); return 0; }
int mkfifo(const char *p, mode_t m) { (void)p; (void)m; errno = ENOSYS; return -1; }
int mknod(const char *p, mode_t m, dev_t d) { (void)p; (void)m; (void)d; errno = ENOSYS; return -1; }
char *ttyname(int fd) { return isatty(fd) ? "/dev/tty" : NULL; }
int lstat(const char *p, struct stat *st) { return stat(p, st); }

/* sys/utsname.h・sys/statvfs.h・sys/random.h は include/ に置いた (newlib に無い) */
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <ctype.h>
int uname(struct utsname *u)
{
    int v = _dos_vernum();
    char *h = getenv("HOSTNAME");
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Human68k");
    strncpy(u->nodename, h && *h ? h : "x68000", sizeof u->nodename - 1);
    snprintf(u->release, sizeof u->release, "%d.%02d", (v >> 8) & 0xff, v & 0xff);
    strcpy(u->version, "Human68k");
    strcpy(u->machine, "m68k");
    return 0;
}
static int pathdrive(const char *p)
{
    if (p && p[0] && p[1] == ':') return toupper((unsigned char)p[0]) - 'A' + 1;
    return 0;   /* カレントドライブ */
}
static int drvfree(int drv, struct statvfs *s)
{
    struct dos_freeinf fi;
    int r = _dos_dskfre(drv, &fi);
    if (r < 0) { errno = ENOENT; return -1; }
    memset(s, 0, sizeof *s);
    s->f_bsize = s->f_frsize = (unsigned long)fi.sec * fi.byte;   /* 1 クラスタ */
    s->f_blocks = fi.max;
    s->f_bfree = s->f_bavail = fi.free;
    s->f_namemax = 18;
    s->f_type = 0x6868;
    return 0;
}
int statvfs(const char *p, struct statvfs *s) { return drvfree(pathdrive(p), s); }
int fstatvfs(int fd, struct statvfs *s) { (void)fd; return drvfree(0, s); }

int setuid(uid_t u) { (void)u; return 0; }
int setgid(gid_t g) { (void)g; return 0; }
int setreuid(uid_t r, uid_t e) { (void)r; (void)e; return 0; }
int setregid(gid_t r, gid_t e) { (void)r; (void)e; return 0; }
int setresuid(uid_t r, uid_t e, uid_t s) { (void)r; (void)e; (void)s; return 0; }
int setresgid(gid_t r, gid_t e, gid_t s) { (void)r; (void)e; (void)s; return 0; }
long pathconf(const char *p, int n) { (void)p; (void)n; return 255; }
long fpathconf(int fd, int n) { (void)fd; (void)n; return 255; }
/* select は下 (端末の節) にある */

/* スレッドは使わない (sort は CPU 1 つなので作らない。作ろうとしたら断る) */
#include <pthread.h>
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)m; (void)a; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)c; (void)a; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return 0; }
int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) { (void)t; (void)a; (void)f; (void)arg; return EAGAIN; }
int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return ESRCH; }

/* exec*: Human68k には exec が無いので、_EXEC で子として動かし、その終了コードで自分も終わる。
   名前は PATH から探す (拡張子が無ければ .x、.r の順) */
static int endsep(const char *p);
static int findprog(const char *name, char *out, size_t n)
{
    static const char *ext[] = { "", ".x", ".r", NULL };
    struct stat st;
    const char *path, *p;
    int i;
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
        for (i = 0; ext[i]; i++) {
            snprintf(out, n, "%s%s", name, ext[i]);
            if (stat(out, &st) == 0 && !S_ISDIR(st.st_mode)) return 0;
        }
        return -1;
    }
    path = getenv("PATH");
    if (!path) path = "";
    /* 区切りは ; と : (zosh と同じ)。ただし要素の先頭の "A:" のようなドライブ名の : は区切りではない */
    for (p = path; ; ) {
        const char *e = p;
        char dir[256];
        size_t k;
        if (isalpha((unsigned char)p[0]) && p[1] == ':') e = p + 2;
        while (*e && *e != ';' && *e != ':') e++;
        k = (size_t)(e - p) < sizeof dir ? (size_t)(e - p) : sizeof dir - 1;
        memcpy(dir, p, k); dir[k] = 0;
        for (i = 0; ext[i]; i++) {
            if (!k) snprintf(out, n, "%s%s", name, ext[i]);
            else snprintf(out, n, "%s%s%s%s", dir, endsep(dir) ? "" : "/", name, ext[i]);
            if (stat(out, &st) == 0 && !S_ISDIR(st.st_mode)) return 0;
        }
        if (!*e) break;
        p = e + 1;
    }
    return -1;
}
/* 引数を 1 行のコマンドラインにする (空白や引用符を含むものは "..." で囲む)。255 バイトまで */
static int mkcomline(char *const argv[], struct dos_comline *cl)
{
    size_t len = 0;
    int i;
    for (i = 1; argv[i]; i++) {
        const char *a = argv[i];
        int q = *a == 0 || strpbrk(a, " \t\"'") != NULL;
        size_t al = strlen(a) + (q ? 2 : 0) + 1;
        if (len + al > 255) { errno = E2BIG; return -1; }
        cl->buffer[len++] = ' ';
        if (q) cl->buffer[len++] = strchr(a, '"') ? '\'' : '"';
        memcpy(cl->buffer + len, a, strlen(a)); len += strlen(a);
        if (q) cl->buffer[len++] = strchr(a, '"') ? '\'' : '"';
    }
    cl->buffer[len] = 0;
    cl->len = len;
    return 0;
}
/* 子に渡す環境: newlib の environ (setenv で変えたもの) を Human68k の形 (先頭 4 バイトが全体の大きさ、
   "名前=値\0" の並び、空の文字列で終わり) にする。0 を渡すと親の DOS の環境がそのまま渡り、env FOO=bar が効かない */
extern char **environ;
static char *mkenv(void)
{
    size_t n = 4 + 1, k;
    char **e, *b, *p;
    for (e = environ; e && *e; e++) n += strlen(*e) + 1;
    n = (n + 1) & ~1;
    if (!(b = malloc(n))) return NULL;
    *(long *)b = n;
    for (p = b + 4, e = environ; e && *e; e++) { k = strlen(*e) + 1; memcpy(p, *e, k); p += k; }
    *p = 0;
    return b;
}
/* 子として動かして終了コードを返す (spawn 相当)。失敗は -1 */
int x68_spawnvp(const char *file, char *const argv[])
{
    static char full[256];
    struct dos_comline cl;
    char *env;
    int r;
    if (findprog(file, full, sizeof full) < 0) { errno = ENOENT; return -1; }
    if (mkcomline(argv, &cl) < 0) return -1;
    if (!(env = mkenv())) { errno = ENOMEM; return -1; }
    fflush(NULL);
    r = _dos_loadexec(full, &cl, env);
    free(env);
    if (r < 0 && (unsigned)r >= 0xffffff00u) { errno = r == -2 ? ENOENT : ENOEXEC; return -1; }
    return r & 0xff;
}
int execvp(const char *file, char *const argv[])
{
    int r = x68_spawnvp(file, argv);
    if (r < 0) return -1;
    exit(r);
}
int execv(const char *path, char *const argv[]) { return execvp(path, argv); }

/* プログラム名: argv[0] は "A:\USR\BIN\SORT.X" の形なので、エラーや使い方に出す名前は "sort" にする
   (gnulib の getprogname と set_program_name をこれに向ける。Shift_JIS の 2 バイト目の \ は区切りにしない) */
extern char **__argv;
const char *x68_progname(const char *argv0)
{
    static char name[32];
    const char *p, *base;
    char *d;
    if (!argv0 && __argv) argv0 = __argv[0];
    if (!argv0) return "?";
    for (p = base = argv0; *p; p++) {
        unsigned char c = *p;
        if ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) { if (p[1]) p++; continue; }
        if (c == '\\' || c == '/' || c == ':') base = p + 1;
    }
    for (d = name; *base && d < name + sizeof name - 1; base++) *d++ = tolower((unsigned char)*base);
    *d = 0;
    if (d - name > 2 && (strcmp(d - 2, ".x") == 0 || strcmp(d - 2, ".r") == 0)) d[-2] = 0;
    return name;
}

/* stat / fstat の st_dev と st_ino: libx68k は入れないので、GNU の「同じファイルか」の判定 (cat の
   "input file is output file"、cp の "are the same file" など) が当てにならない。
   -Wl,--wrap=stat,--wrap=fstat で包み、_NAMECK で正規化したフルパスのハッシュを st_ino、ドライブを st_dev にする。
   名前のわからない番号 (シェルから受けた標準入出力) は番号ごとに別の値にする */
/* stat / fstat そのもの: libx68k の版は DOS の日時を mktime で time_t にするので、mktime → tzset → sscanf → strtod が
   どのプログラムにも入る (約 12KB)。日時は計算で直す。TZ は "JST-9" のような形の時差だけ見る (夏時間は見ない)。
   TZ が無ければ、elf2x68k の newlib と同じく JST-9 */
static time_t dos2time(unsigned date, unsigned tm)
{
    long y = (date >> 9) + 1980, m = (date >> 5) & 15, d = date & 31, era, yoe, doy, doe, days;
    const char *tz = getenv("TZ");
    time_t t;
    if (!tz || !*tz) tz = "JST-9";   /* elf2x68k の newlib は TZ が無いと日本時間 (JST-9) として扱う。それに合わせる */
    if (m < 1 || m > 12) m = 1;
    if (d < 1) d = 1;
    y -= m <= 2;   /* 1970-01-01 からの日数 (グレゴリオ暦) */
    era = y / 400; yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    t = (time_t)days * 86400 + (tm >> 11) * 3600 + ((tm >> 5) & 63) * 60 + (tm & 31) * 2;
    if (tz) {   /* "JST-9" "EST5" "UTC" "XXX-9:30": 英字の後の符号付きの時 (と分) が、UTC から引く時間 */
        int sign = 1, h = 0, mi = 0;
        if (*tz == '<') { while (*tz && *tz != '>') tz++; if (*tz) tz++; }   /* "<+09>-9" の形: <> の中は名前 */
        while (*tz && !(*tz == '+' || *tz == '-' || (*tz >= '0' && *tz <= '9'))) tz++;
        if (*tz == '-') { sign = -1; tz++; } else if (*tz == '+') tz++;
        while (*tz >= '0' && *tz <= '9') h = h * 10 + (*tz++ - '0');
        if (*tz == ':') { tz++; while (*tz >= '0' && *tz <= '9') mi = mi * 10 + (*tz++ - '0'); }
        t += sign * (h * 3600L + mi * 60L);
    }
    return t;
}
static int lean_stat(const char *path, struct stat *st)
{
    struct dos_filbuf fb;
    if (strpbrk(path, "*?")) { errno = ENOENT; return -1; }
    if (_dos_files(&fb, path, 0x37) < 0) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    st->st_mode = (fb.atr & 0x10) ? (S_IFDIR | 0777) : (S_IFREG | ((fb.atr & 1) ? 0444 : 0666));
    if (!(fb.atr & 0x10)) {   /* libx68k と同じく、.x と .z は実行できるファイル (ls -l、test -x、find -perm) */
        size_t n = strlen(path);
        if (n > 2 && path[n - 2] == '.' && strchr("xXzZ", path[n - 1])) st->st_mode |= 0111;
    }
    st->st_nlink = 1;
    st->st_size = (fb.atr & 0x10) ? 0 : fb.filelen;
    st->st_blksize = 512;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_mtime = st->st_atime = st->st_ctime = dos2time(fb.date, fb.time);
    return 0;
}
static int lean_fstat(int fd, struct stat *st)
{
    int dev = _dos_ioctrlgt(fd);
    if (dev < 0) { errno = EBADF; return -1; }
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_blksize = 512;
    if (dev & 0x80) {   /* キャラクタデバイス (CON・NUL・AUX など) */
        st->st_mode = S_IFCHR | 0666;
    } else {
        long cur = _dos_seek(fd, 0, 1), end = _dos_seek(fd, 0, 2);
        unsigned long dt = (unsigned long)_dos_filedate(fd, 0);
        _dos_seek(fd, cur, 0);
        st->st_mode = S_IFREG | 0666;
        st->st_size = end < 0 ? 0 : end;
        st->st_blocks = (st->st_size + 511) / 512;
        st->st_mtime = st->st_atime = st->st_ctime = dos2time(dt >> 16, dt & 0xffff);
    }
    return 0;
}
static void setino(const char *path, struct stat *st, int fd)
{
    struct dos_nameckbuf nb;
    unsigned long h = 5381;
    const char *p;
    if (path && _dos_nameck(path, &nb) >= 0) {
        for (p = nb.drive; p < nb.drive + 2; p++) h = h * 33 + toupper((unsigned char)*p);
        for (p = nb.path; *p; p++) h = h * 33 + toupper((unsigned char)*p);
        for (p = nb.name; *p; p++) h = h * 33 + toupper((unsigned char)*p);
        for (p = nb.ext; *p; p++) h = h * 33 + toupper((unsigned char)*p);
        st->st_dev = toupper((unsigned char)nb.drive[0]) - 'A' + 1;
        st->st_ino = (ino_t)(((h ^ (h >> 15)) & 0x7fff) | 1);   /* newlib の ino_t は 16 ビット */
    } else {
        st->st_dev = 0x100 + (fd < 0 ? 0 : fd);
        st->st_ino = (ino_t)(0x8000 | (fd & 0xff));
    }
}
/* ルート ("A:" "A:/" "/" など) は _FILES で引けず libx68k の stat が失敗するので、ディレクトリとして作る */
static int isroot(const char *p)
{
    if (!*p) return 0;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '/' || *p == '\\') p++;
    return *p == 0;
}
static int dotname(const char *p)   /* 最後の要素が . か .. */
{
    size_t n = strlen(p);
    if (n >= 1 && p[n - 1] == '.' && (n == 1 || p[n - 2] == '/' || p[n - 2] == '\\' || p[n - 2] == ':')) return 1;
    if (n >= 2 && p[n - 1] == '.' && p[n - 2] == '.' && (n == 2 || p[n - 3] == '/' || p[n - 3] == '\\' || p[n - 3] == ':')) return 1;
    return 0;
}
static int chdir_drive(const char *p);
char *__real_getcwd(char *, size_t);
int __wrap_stat(const char *path, struct stat *st)
{
    int r = lean_stat(path, st);
    /* "." ".." "D1/." は _FILES で引けないので、そこへ移ってフルパスを得てから引く */
    if (r < 0 && path && dotname(path)) {
        char old[256], abs[256];
        int e = errno;
        if (__real_getcwd(old, sizeof old) && chdir_drive(path) == 0) {
            if (!__real_getcwd(abs, sizeof abs)) abs[0] = 0;
            chdir_drive(old);
            if (abs[0]) return __wrap_stat(abs, st);
        }
        errno = e;
    }
    /* ドライブの状態の bit1 = メディアが入っている (空の FD を読みに行くと「中止 再実行 無視」の枠が出る) */
    if (r < 0 && path && isroot(path) && (path[0] && path[1] == ':' ? (_dos_drvctrl(0, toupper((unsigned char)path[0]) - 'A' + 1) & 0x80000002) == 2 : 1)) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 1;
        r = 0;
    }
    if (r == 0) setino(path, st, -1);
    return r;
}
static const char *dirfd_path(int fd);
int __wrap_fstat(int fd, struct stat *st)
{
    int r;
    if (dirfd_path(fd)) return __wrap_stat(dirfd_path(fd), st);
    r = lean_fstat(fd, st);
    if (r == 0) setino(fd >= 0 && fd < 64 ? __fd_list[fd].name : NULL, st, fd);
    return r;
}

/* close: 標準入出力 (0〜4) は親 (シェル) と同じ DOS の番号なので、子が閉じると親の出力まで閉じる。
   GNU の道具は終わるときに close_stdout で標準出力を閉じるので、0〜4 は閉じたことにして実際には閉じない
   (-Wl,--wrap=close。終了時に Human68k が後始末する) */
int __real_close(int);
static int dirfd_close(int fd);
static void fd_forget(int fd);
int __wrap_close(int fd)
{
    if (fd >= 0 && fd <= 4) return 0;
    if (fd == 3000) return 0;   /* /dev/tty の仮の番号 (TTYFD) */
    fd_forget(fd);
    if (dirfd_close(fd) == 0) return 0;
    return __real_close(fd);
}

/* 0〜2 の差し替え: 子が DOS の _DUP2 で 0 を差し替えると、戻しても親のシェルの標準出力が CON に戻ってしまう
   (XEiJ で確かめた)。そこで gnulib の fd_reopen (touch・csplit・split・dd) は差し替えずに新しい番号を返し、
   STDIN_FILENO などをこの表に向ける (scripts/gnulib_x68k.py が lib/unistd.h と lib/fd-reopen.c を直す) */
int x68_stdfd[3] = { 0, 1, 2 };

/* getrandom (shuf・sort -R・mktemp): libx68k の getentropy は ENOSYS を返すだけなので、
   起動からの時間 (1/100 秒)・日時・スタックの番地を種にした xorshift で埋める (暗号には使えない) */
#include <sys/random.h>
#include <x68k/iocs.h>
ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    static unsigned long x;
    unsigned char *p = buf;
    size_t i;
    (void)flags;
    if (!x) x = (unsigned long)_iocs_ontime().sec * 2654435761u ^ (unsigned long)time(NULL) ^ (unsigned long)&i ^ 0x9e3779b9u;
    for (i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[i] = x >> 24;
    }
    return len;
}

/* newlib の strtold は m68k の 96 ビットの long double で桁を間違える ("3.14159" が 3.24224 になる)。
   strtod (64 ビット) で読んで広げる (-Wl,--wrap=strtold。printf・seq・sort -g が使う) */
long double __wrap_strtold(const char *s, char **end) { return strtod(s, end); }

/* fcntl: libx68k のは ENOSYS を返すだけ。gnulib の stdopen (gzip・find・diff) は F_GETFD で 0〜2 が
   開いているかを見て、失敗すると "standard file descriptors" で止まる */
#include <stdarg.h>
int __wrap_dup(int);
static int fdopen_p(int fd)
{
    if (fd >= 0 && fd <= 4) return 1;
    if (fd == 3000 || dirfd_path(fd)) return 1;
    return fd > 4 && fd < 64 && (__fd_list[fd].flags & 0x80000000L);
}
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;
    va_start(ap, cmd); arg = va_arg(ap, int); va_end(ap);
    if (!fdopen_p(fd)) { errno = EBADF; return -1; }
    switch (cmd) {
    case F_GETFD: return 0;
    case F_SETFD: return 0;
    case F_GETFL: return fd <= 4 ? O_RDWR : (int)(__fd_list[fd].flags & (O_ACCMODE | O_APPEND));
    case F_SETFL: return 0;
    case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
    case F_DUPFD_CLOEXEC:
#endif
        { int n = __wrap_dup(fd); (void)arg; return n; }
    default: errno = EINVAL; return -1;
    }
}

/* getcwd(NULL, 0) (POSIX の拡張。find の save_cwd が使う) を、libx68k は受け付けないので領域を取って渡す (-Wl,--wrap=getcwd) */
char *__real_getcwd(char *, size_t);
char *__wrap_getcwd(char *buf, size_t size)
{
    char *r, *own = NULL;
    if (!buf) {
        if (!size) size = 256;
        if (!(buf = own = malloc(size))) { errno = ENOMEM; return NULL; }
    }
    r = __real_getcwd(buf, size);
    if (!r) free(own);
    return r;
}

/* ディレクトリの番号: Human68k はディレクトリを開けないが、gnulib の fts (find・du・rm -r・cp -r) や
   save_cwd は open(".") や fdopendir・fchdir を使う。ディレクトリを読み込みで開こうとしたら、
   フルパスを覚えた仮の番号 (1000〜) を返し、close・fstat・fchdir・dup・fcntl をそれで受ける
   (-Wl,--wrap=open,--wrap=dup。gnulib の openat・fstatat・fdopendir は fchdir で動く) */
#define DIRFD0 1000
#define NDIRFD 64
static char *dirfds[NDIRFD];
static const char *dirfd_path(int fd)
{
    return fd >= DIRFD0 && fd < DIRFD0 + NDIRFD ? dirfds[fd - DIRFD0] : NULL;
}
static int dirfd_new(const char *abs)
{
    int i;
    for (i = 0; i < NDIRFD; i++)
        if (!dirfds[i]) {
            if (!(dirfds[i] = strdup(abs))) { errno = ENOMEM; return -1; }
            return DIRFD0 + i;
        }
    errno = EMFILE;
    return -1;
}
static int dirfd_close(int fd)
{
    if (!dirfd_path(fd)) return -1;
    free(dirfds[fd - DIRFD0]); dirfds[fd - DIRFD0] = NULL;
    return 0;
}
/* ドライブも移る chdir (_CHDIR は今のドライブを変えない) */
static void save_start_cwd(void);
static int chdir_drive(const char *p)
{
    save_start_cwd();   /* ドライブを移す前に、戻る先を覚える */
    if (p[0] && p[1] == ':') _dos_chgdrv(toupper((unsigned char)p[0]) - 'A');
    return chdir(p);
}
#define TTYFD 3000
static int is_ttyname(const char *p);
static void tty_opened(void);
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    int mode, r, e;
    struct stat st;
    va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    if (path && is_ttyname(path)) { tty_opened(); return TTYFD; }
    r = __real_open(path, flags & ~O_DIRECTORY, mode);
    if (r >= 0 || (flags & O_ACCMODE) != O_RDONLY) return r;
    e = errno;
    if (__wrap_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char old[256], abs[256];
        if (!__real_getcwd(old, sizeof old)) { errno = e; return -1; }
        if (chdir_drive(path) < 0) { errno = e; return -1; }
        if (!__real_getcwd(abs, sizeof abs)) abs[0] = 0;
        chdir_drive(old);
        if (!abs[0]) { errno = e; return -1; }
        return dirfd_new(abs);
    }
    errno = e;
    return r;
}
int __real_dup(int);
int __wrap_dup(int fd)
{
    if (dirfd_path(fd)) return dirfd_new(dirfd_path(fd));
    return __real_dup(fd);
}
int fchdir(int fd)
{
    if (!dirfd_path(fd)) { errno = fd >= 0 && fd < 64 ? ENOTDIR : EBADF; return -1; }
    return chdir_drive(dirfd_path(fd));
}

/* *at 関数と fdopendir / dirfd: 上の仮の番号のフルパスに名前をつないで、ふつうの関数を呼ぶ。
   gnulib の代わりの版 (fchdir で移って戻る) は fts の中で途中で止まったので、こちらで用意する */
#include <dirent.h>
/* 最後の文字が区切り (\ か /) か。Shift_JIS の 2 バイト目の 5C (表 = 95 5C) は区切りではない */
static int endsep(const char *p)
{
    const unsigned char *q = (const unsigned char *)p;
    int sep = 0;
    for (; *q; q++) {
        if (((*q >= 0x81 && *q <= 0x9f) || (*q >= 0xe0 && *q <= 0xfc)) && q[1]) { q++; sep = 0; continue; }
        sep = *q == '\\' || *q == '/';
    }
    return sep;
}
static int atpath(int dfd, const char *name, char *out, size_t n)
{
    const char *base;
    size_t k;
    if (dfd == AT_FDCWD || name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':')) {
        if (strlen(name) >= n) { errno = ENAMETOOLONG; return -1; }
        strcpy(out, name);
        return 0;
    }
    if (!(base = dirfd_path(dfd))) { errno = EBADF; return -1; }
    k = strlen(base);
    if (k + 1 + strlen(name) >= n) { errno = ENAMETOOLONG; return -1; }
    strcpy(out, base);
    if (k && !endsep(base)) out[k++] = '\\';
    strcpy(out + k, name);
    return 0;
}
int openat(int dfd, const char *name, int flags, ...)
{
    char p[256];
    va_list ap;
    int mode;
    va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    if (atpath(dfd, name, p, sizeof p) < 0) return -1;
    return __wrap_open(p, flags, mode);
}
int fstatat(int dfd, const char *name, struct stat *st, int flag)
{
    char p[256];
    (void)flag;
    if (atpath(dfd, name, p, sizeof p) < 0) return -1;
    return __wrap_stat(p, st);
}
int unlinkat(int dfd, const char *name, int flag)
{
    char p[256];
    if (atpath(dfd, name, p, sizeof p) < 0) return -1;
    return (flag & AT_REMOVEDIR) ? rmdir(p) : unlink(p);
}
int mkdirat(int dfd, const char *name, mode_t m)
{
    char p[256];
    if (atpath(dfd, name, p, sizeof p) < 0) return -1;
    return mkdir(p, m);
}
/* DIR と番号の対応 (opendir で開いたものはフルパスを覚えておき、dirfd を聞かれたら仮の番号を作る) */
#define NDIRMAP 64
static struct { DIR *d; int fd; char *path; int empty; } dirmap[NDIRMAP];
static void dirmap_add(DIR *d, int fd, const char *path)
{
    int i;
    for (i = 0; i < NDIRMAP; i++)
        if (!dirmap[i].d) { dirmap[i].d = d; dirmap[i].fd = fd; dirmap[i].path = path ? strdup(path) : NULL; dirmap[i].empty = 0; return; }
}
static int dirmap_find(DIR *d)
{
    int i;
    for (i = 0; i < NDIRMAP; i++) if (dirmap[i].d == d) return i;
    return -1;
}
/* 空のディレクトリ: libx68k の opendir は _FILES が何も見つけないと失敗する (find・du・rm -r が止まる)。
   空の DIR の代わりを返し、readdir はすぐ終わりにする */
static DIR *emptydir(void)
{
    DIR *d = malloc(4);
    int i;
    if (!d) return NULL;
    dirmap_add(d, -1, NULL);
    if ((i = dirmap_find(d)) >= 0) dirmap[i].empty = 1;
    return d;
}
DIR *__real_opendir(const char *);
DIR *__wrap_opendir(const char *path)
{
    DIR *d = __real_opendir(path);
    struct stat st;
    if (!d && __wrap_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) d = emptydir();
    if (d) {
        char old[256], abs[256];
        abs[0] = 0;
        if (__real_getcwd(old, sizeof old) && chdir_drive(path) == 0) {
            if (!__real_getcwd(abs, sizeof abs)) abs[0] = 0;
            chdir_drive(old);
        }
        {   /* emptydir() は登録済みなので、その項目にパスを入れる */
            int i = dirmap_find(d);
            if (i >= 0) { if (abs[0] && !dirmap[i].path) dirmap[i].path = strdup(abs); }
            else dirmap_add(d, -1, abs[0] ? abs : NULL);
        }
    }
    return d;
}
DIR *fdopendir(int fd)
{
    DIR *d;
    if (!dirfd_path(fd)) { errno = fd >= 0 && fd < 64 ? ENOTDIR : EBADF; return NULL; }
    if ((d = __real_opendir(dirfd_path(fd))) != NULL) { dirmap_add(d, fd, NULL); return d; }
    if (!(d = emptydir())) return NULL;
    dirmap[dirmap_find(d)].fd = fd;
    return d;
}
int dirfd(DIR *d)
{
    int i;
    for (i = 0; i < NDIRMAP; i++)
        if (dirmap[i].d == d) {
            if (dirmap[i].fd < 0 && dirmap[i].path) dirmap[i].fd = dirfd_new(dirmap[i].path);
            return dirmap[i].fd;
        }
    errno = EINVAL;
    return -1;
}
int __real_closedir(DIR *);
int __wrap_closedir(DIR *d)
{
    int i = dirmap_find(d), empty = 0;
    if (i >= 0) {
        if (dirmap[i].fd >= 0) dirfd_close(dirmap[i].fd);
        free(dirmap[i].path);
        empty = dirmap[i].empty;
        dirmap[i].d = NULL; dirmap[i].path = NULL; dirmap[i].empty = 0;
    }
    if (empty) { free(d); return 0; }
    return __real_closedir(d);
}
struct dirent *__real_readdir(DIR *);
struct dirent *__wrap_readdir(DIR *d)
{
    int i = dirmap_find(d);
    if (i >= 0 && dirmap[i].empty) return NULL;
    return __real_readdir(d);
}
void __real_rewinddir(DIR *);
void __wrap_rewinddir(DIR *d)
{
    int i = dirmap_find(d);
    if (i >= 0 && dirmap[i].empty) return;
    __real_rewinddir(d);
}

/* カレントディレクトリ: Human68k ではドライブごとに 1 つで、子が chdir すると親 (シェル) まで移る。
   UNIX と同じく自分だけの話にするため、最初の chdir の前に今のドライブとそのディレクトリを覚え、
   exit で戻す (-Wl,--wrap=chdir,--wrap=exit。crt0 の atexit は constructor より後で初期化されるので使わない) */
static char start_cwd[256];
static int start_saved;
int __real_chdir(const char *);
static void save_start_cwd(void)
{
    if (!start_saved) {
        start_saved = 1;
        if (!__real_getcwd(start_cwd, sizeof start_cwd)) start_cwd[0] = 0;
    }
}
int __wrap_chdir(const char *p)
{
    save_start_cwd();
    return __real_chdir(p);
}
void __real_exit(int) __attribute__((noreturn));
static void tty_closeall(void);
void __wrap_exit(int st)
{
    tty_closeall();
    if (start_saved && start_cwd[0]) {
        start_saved = 0;
        if (start_cwd[1] == ':') _dos_chgdrv(toupper((unsigned char)start_cwd[0]) - 'A');
        __real_chdir(start_cwd);
    }
    __real_exit(st);
}

/* 仮のディレクトリの番号を read したら EISDIR (cat DIR が "Is a directory" と出す) (-Wl,--wrap=read) */
ssize_t __real_read(int, void *, size_t);
/* 番号ごとの isatty の控え (DOS の _IOCTRL を毎回呼ばない)。close で忘れる */
static signed char isatty_cache[64];   /* 0 = まだ、1 = 端末でない、2 = 端末 */
static int fd_isatty(int fd)
{
    if (!isatty_cache[fd]) isatty_cache[fd] = isatty(fd) ? 2 : 1;
    return isatty_cache[fd] == 2;
}
static void fd_forget(int fd) { if (fd >= 0 && fd < 64) isatty_cache[fd] = 0; }
/* DOS の読み書きのエラー: -6 開いていない番号、-5 そのモードでは使えない (読み込み専用への書き込みなど)、-23 ディスクがいっぱい */
static int dos2errno_rw(int r) { return r == -6 ? EBADF : r == -5 ? EACCES : r == -23 ? ENOSPC : EIO; }
/* dup2 で番号の中身が替わるので、isatty の控えを忘れる (-Wl,--wrap=dup2) */
int __real_dup2(int, int);
int __wrap_dup2(int oldfd, int newfd) { fd_forget(newfd); return __real_dup2(oldfd, newfd); }
static ssize_t tty_read(void *b, size_t n);
static unsigned char tty_raw[3];   /* tcsetattr で ICANON を外した標準入力 (vim などはこれを生で読む) */
ssize_t __wrap_read(int fd, void *b, size_t n)
{
    if (dirfd_path(fd)) { errno = EISDIR; return -1; }
    if (fd == TTYFD || (fd >= 0 && fd <= 2 && tty_raw[fd])) return tty_read(b, n);
    if (fd >= 0 && fd < 64 && n > 0 && !(__fd_list[fd].flags & O_BINARY) && !fd_isatty(fd)) {
        /* テキストのファイル・パイプ: libx68k は CR と ^Z を 1 バイトずつ抜き、要った分を満たすまで小さい _READ を
           くり返し、そのたびに isatty を呼ぶ。ここでは 1 回読んで抜くだけにする (抜いて空になったら次を読む) */
        char *p = b;
        for (;;) {
            int r = _dos_read(fd, p, n), i, k;
            if (r < 0) { errno = dos2errno_rw(r); return -1; }
            if (r == 0) return 0;
            for (i = k = 0; i < r; i++) if (p[i] != '\r' && p[i] != 0x1a) p[k++] = p[i];
            if (k) return k;
        }
    }
    return __real_read(fd, b, n);
}

/* ---- 端末: less などのキー入力と termcap ----
   /dev/tty (と /dev/con) を読み込みで開くと仮の番号 TTYFD を返し、read はキーを 1 つずつ生のまま返す
   (エコーしない、行にまとめない)。キーは zosh と同じく、DOS のキーの列 (貼り付け・ssh などの遠隔) に
   あればそれを、無ければ IOCS の _B_KEYINP で読む。矢印などはスキャンコードを VT100 の列に直す */
#include <x68k/iocs.h>
static int is_ttyname(const char *p)
{
    return !strcasecmp(p, "/dev/tty") || !strcasecmp(p, "/dev/con");
}
/* /dev/tty を開いたら、Human68k の BREAK の確かめ (Ctrl+C で DOS がプログラムを止める) を切る。
   Ctrl+C はキーの 3 として less などに届き、less は後片付け (反転の解除など) をしてから戻る。終わるときに元に戻す */
static int tty_brk = -1, con_fd = -1;
static void tty_opened(void)
{
    if (tty_brk < 0) { tty_brk = _dos_breakck(-1); _dos_breakck(0); }
}
static void tty_closeall(void)
{
    if (tty_brk >= 0) { _dos_breakck(tty_brk); tty_brk = -1; }
    if (con_fd >= 0) { _dos_close(con_fd); con_fd = -1; }
}
/* 書き込みは CON (画面) に直に */
static ssize_t tty_write(const void *b, size_t n)
{
    int r;
    if (con_fd < 0 && (con_fd = _dos_open("CON", 1)) < 0) { con_fd = -1; errno = EIO; return -1; }
    r = _dos_write(con_fd, b, n);
    if (r < 0) { errno = EIO; return -1; }
    return r;
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *b, size_t n)
{
    if (fd == 3000) return tty_write(b, n);   /* TTYFD */
    if (fd >= 0 && fd < 64 && n > 0 && !(__fd_list[fd].flags & O_BINARY) && !fd_isatty(fd)) {
        /* テキストのファイル・パイプ: LF の前に CR を足す。libx68k は 256 バイトずつ _WRITE するので、4KB ずつにする */
        static char buf[4096 + 2];
        const char *p = b;
        size_t i = 0;
        while (i < n) {
            int k = 0, r;
            while (i < n && k < 4096) { if (p[i] == '\n') buf[k++] = '\r'; buf[k++] = p[i++]; }
            r = _dos_write(fd, buf, k);
            if (r < 0) { errno = dos2errno_rw(r); return -1; }
            if (r < k) { errno = ENOSPC; return -1; }
        }
        return n;
    }
    return __real_write(fd, b, n);
}
/* fwrite: 小さな書き込みのたびに newlib の __sfvwrite_r を通ると重い (join・cut など欄ごとに呼ぶ)。
   全部バッファに入る書き込みは、ここで写して済ませる。行バッファ・バッファ無し・文字列・ワイド文字の流れは newlib に任せる
   (-Wl,--wrap=fwrite,--wrap=fwrite_unlocked) */
static int fw_fast(const void *b, size_t len, FILE *fp)
{
    const char *q = b;
    unsigned char *p;
    if ((fp->_flags & (__SWR | __SLBF | __SNBF | __SSTR | __SERR | __SORD)) != (__SWR | __SORD)
        || (fp->_flags2 & __SWID) || fp->_w <= 0 || (size_t)fp->_w <= len)
        return 0;
    p = fp->_p;
    fp->_p += len; fp->_w -= len;
    while (len--) *p++ = *q++;
    return 1;
}
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __real_fwrite_unlocked(const void *, size_t, size_t, FILE *);
size_t __wrap_fwrite(const void *b, size_t s, size_t n, FILE *fp)
{
    if (s == 1 ? fw_fast(b, n, fp) : n == 1 && fw_fast(b, s, fp)) return n;
    return __real_fwrite(b, s, n, fp);
}
size_t __wrap_fwrite_unlocked(const void *b, size_t s, size_t n, FILE *fp)
{
    if (s == 1 ? fw_fast(b, n, fp) : n == 1 && fw_fast(b, s, fp)) return n;
    return __real_fwrite_unlocked(b, s, n, fp);
}
static unsigned char ttyq[8];
static int ttyqn, ttyqi;
static void ttyput(const char *k) { ttyqn = strlen(k); ttyqi = 0; memcpy(ttyq, k, ttyqn); }
static ssize_t tty_read(void *b, size_t n)
{
    unsigned char *o = b;
    if (!n) return 0;
    while (ttyqi >= ttyqn) {
        long p = _iocs_b_lpeek((const void *)0x12406);
        int k, c, sc;
        fflush(stdout);
        if (p >= 0x1240a && p < 0x1242a && _iocs_b_bpeek((const void *)p) && _dos_keysns()) {
            ttyq[0] = _dos_inkey() & 0xff; ttyqn = 1; ttyqi = 0;   /* 遠隔のキーは送られたまま (ESC [ A など) */
            break;
        }
        k = _iocs_b_keyinp(); c = k & 0xff; sc = (k >> 8) & 0x7f;
        switch (sc) {
        case 0x3b: ttyput("\033[D"); continue;   /* ← */
        case 0x3c: ttyput("\033[A"); continue;   /* ↑ */
        case 0x3d: ttyput("\033[C"); continue;   /* → */
        case 0x3e: ttyput("\033[B"); continue;   /* ↓ */
        case 0x36: ttyput("\033[H"); continue;   /* HOME */
        case 0x3a: ttyput("\033[F"); continue;   /* UNDO を END の代わりに */
        case 0x38: ttyput("\033[6~"); continue;  /* ROLL UP: 次のページ */
        case 0x39: ttyput("\033[5~"); continue;  /* ROLL DOWN: 前のページ */
        case 0x37: ttyput("\177"); continue;     /* DEL */
        }
        if (!c) continue;
        ttyq[0] = c; ttyqn = 1; ttyqi = 0;
    }
    *o = ttyq[ttyqi++];
    return 1;
}
#include <termios.h>
int tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    memset(t, 0, sizeof *t);
    t->c_lflag = ICANON | ECHO | ISIG;
    t->c_cc[VERASE] = 8; t->c_cc[VKILL] = 21; t->c_cc[VWERASE] = 23; t->c_cc[VINTR] = 3; t->c_cc[VEOF] = 4;
    t->c_ospeed = t->c_ispeed = B38400;
    return 0;
}
/* ICANON を外すと、その番号 (0〜2 で端末のもの) の read を tty_read (生のキー) にする。戻すと DOS の行入力に戻る */
int tcsetattr(int fd, int act, const struct termios *t)
{
    (void)act;
    if (fd >= 0 && fd <= 2 && isatty(fd)) { tty_raw[fd] = !(t->c_lflag & ICANON); if (tty_raw[fd]) tty_opened(); }
    return 0;
}
/* キーが来ているか (読み残し、DOS のキーの列、IOCS のキーの列) */
static int tty_avail(void)
{
    long p;
    if (ttyqi < ttyqn) return 1;
    p = _iocs_b_lpeek((const void *)0x12406);
    if (p >= 0x1240a && p < 0x1242a && _iocs_b_bpeek((const void *)p) && _dos_keysns()) return 1;
    return _iocs_b_keysns() != 0;
}
static int is_ttyfd(int fd) { return fd == TTYFD || (fd >= 0 && fd <= 2 && tty_raw[fd]); }
/* select / poll: 端末の番号は tty_avail で見て、来ていなければ時間まで 10ms ずつ待つ。ほかの番号はいつも「読める・書ける」 */
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    long left = tv ? tv->tv_sec * 1000L + tv->tv_usec / 1000 : -1;
    fd_set rr;
    int fd, cnt, tty = 0, any = 0;
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    if (e) FD_ZERO(e);
    for (fd = 0; fd < n; fd++) if ((r && FD_ISSET(fd, r)) || (w && FD_ISSET(fd, w))) any = 1;
    if (!any) {   /* 見る番号が無ければ、時間だけ待つ (sleep の代わりに select を使うもの) */
        if (left >= 1000) sleep(left / 1000);
        if (left > 0) usleep((left % 1000) * 1000L);
        if (r) FD_ZERO(r);
        if (w) FD_ZERO(w);
        return 0;
    }
    for (;;) {
        cnt = 0;
        FD_ZERO(&rr);
        if (r) for (fd = 0; fd < n; fd++) if (FD_ISSET(fd, r)) {
            if (is_ttyfd(fd)) { tty = 1; if (tty_avail()) { FD_SET(fd, &rr); cnt++; } }
            else { FD_SET(fd, &rr); cnt++; }
        }
        if (w) for (fd = 0; fd < n; fd++) if (FD_ISSET(fd, w)) cnt++;
        if (cnt || !tty || left == 0) break;
        usleep(10000);
        if (left > 0) left = left > 10 ? left - 10 : 0;
    }
    if (r) *r = rr;
    return cnt;
}
#include <poll.h>
int __wrap_poll(struct pollfd *fds, nfds_t nf, int ms)
{
    long left = ms;
    nfds_t i;
    int cnt, tty;
    for (;;) {
        cnt = tty = 0;
        for (i = 0; i < nf; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            if (is_ttyfd(fds[i].fd)) { tty = 1; if ((fds[i].events & POLLIN) && tty_avail()) fds[i].revents = POLLIN; }
            else fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
            if (fds[i].revents) cnt++;
        }
        if (cnt || !tty || left == 0) return cnt;
        usleep(10000);
        if (left > 0) left = left > 10 ? left - 10 : 0;
    }
}
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
int cfsetospeed(struct termios *t, speed_t s) { t->c_ospeed = s; return 0; }
int cfsetispeed(struct termios *t, speed_t s) { t->c_ispeed = s; return 0; }
int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }

/* termcap: TERM によらず、zos.x のペイン (term/vt100) と Human68k の CON の両方が分かる VT100 の一部だけを返す。
   桁数・行数は IOCS の _B_CONSOL (d1 = d2 = -1 で今の枠を返す。zos.x はペインの大きさを返す) から */
char PC, *BC, *UP;
short ospeed;
static void con_size(int *cols, int *rows)
{
    register long d0 __asm__("d0") = 0x2e;
    register long d1 __asm__("d1") = -1;
    register long d2 __asm__("d2") = -1;
    char *e;
    __asm__ volatile ("trap #15" : "+r"(d0), "+r"(d1), "+r"(d2) : : "a0", "a1", "cc", "memory");
    *cols = ((d2 >> 16) & 0xffff) + 1; *rows = (d2 & 0xffff) + 1;
    if (*cols < 20 || *cols > 256) *cols = 96;
    if (*rows < 4 || *rows > 128) *rows = 31;
    if ((e = getenv("COLUMNS")) && atoi(e) > 0) *cols = atoi(e);
    if ((e = getenv("LINES")) && atoi(e) > 0) *rows = atoi(e);
}
int tgetent(char *bp, const char *name) { (void)bp; (void)name; return 1; }
int tgetnum(const char *id)
{
    int c, r;
    con_size(&c, &r);
    if (!strncmp(id, "co", 2)) return c;
    if (!strncmp(id, "li", 2)) return r;
    return -1;
}
int tgetflag(const char *id) { return !strncmp(id, "am", 2) || !strncmp(id, "bs", 2); }
static const char *const tcaps[][2] = {
    { "te", "\033[m" }, { "cl", "\033[H\033[2J" }, { "cm", "\033[%i%d;%dH" }, { "ce", "\033[K" }, { "cd", "\033[J" },
    { "ho", "\033[H" }, { "so", "\033[7m" }, { "se", "\033[m" }, { "us", "\033[4m" }, { "ue", "\033[m" },
    { "md", "\033[1m" }, { "me", "\033[m" }, { "mr", "\033[7m" }, { "le", "\b" }, { "nd", "\033[C" },
    { "up", "\033[A" }, { "do", "\n" }, { "cr", "\r" }, { "al", "\033[L" }, { "dl", "\033[M" },
    { "ku", "\033[A" }, { "kd", "\033[B" }, { "kl", "\033[D" }, { "kr", "\033[C" }, { "kh", "\033[H" },
    { "@7", "\033[F" }, { "kP", "\033[5~" }, { "kN", "\033[6~" }, { "kD", "\177" },
    { NULL, NULL } };
char *tgetstr(const char *id, char **area)
{
    int i;
    for (i = 0; tcaps[i][0]; i++)
        if (!strncmp(id, tcaps[i][0], 2)) {
            const char *v = tcaps[i][1];
            if (area && *area) { char *r = *area; strcpy(r, v); *area += strlen(v) + 1; return r; }
            return (char *)v;
        }
    return NULL;
}
/* cm の %i %d %2 %. %+ %r %% だけ */
char *tgoto(const char *cm, int col, int row)
{
    static char buf[128];
    char *o = buf;
    int a[2] = { row, col }, k = 0, t;
    for (; *cm && o < buf + sizeof buf - 8; cm++) {
        if (*cm != '%') { *o++ = *cm; continue; }
        switch (*++cm) {
        case 'i': a[0]++; a[1]++; break;
        case 'r': t = a[0]; a[0] = a[1]; a[1] = t; break;
        case 'd': o += sprintf(o, "%d", a[k++ & 1]); break;
        case '2': o += sprintf(o, "%02d", a[k++ & 1]); break;
        case '.': *o++ = a[k++ & 1]; break;
        case '+': *o++ = a[k++ & 1] + *++cm; break;
        case '%': *o++ = '%'; break;
        default: break;
        }
    }
    *o = 0;
    return buf;
}
int tputs(const char *str, int affcnt, int (*putc_)(int))
{
    (void)affcnt;
    if (!str) return -1;
    while (*str >= '0' && *str <= '9') str++;   /* 先頭の遅延の数 */
    for (; *str; str++) putc_((unsigned char)*str);
    return 0;
}

/* popen / pclose: パイプも fork も無いので一時ファイルを使う。"r" は COMMAND.X /C "cmd > 一時" を動かしてから
   その一時ファイルを開く。"w" は一時ファイルに書かせておき、pclose で COMMAND.X /C "cmd < 一時" を動かす。
   付け替え (>、<) は子の COMMAND.X がする (自分で標準入出力を _DUP2 しない) */
#define NPOPEN 4
static struct { FILE *f; char tmp[96]; char *cmd; int w, st; } popens[NPOPEN];
/* 一時ファイルの名前: TEMP (無ければ TMP、それも無いか長すぎれば A:/tmp) に、8+3 に収まる "pK%04x%d.tmp" */
static void tmpname_x(char *b, size_t n, int kind, int i)
{
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t || strlen(t) + 16 > n) t = "A:/tmp";
    snprintf(b, n, "%s%sp%c%04x%d.tmp", t, endsep(t) ? "" : "/", kind,
             (unsigned)((unsigned long)_dos_getpdb() >> 4) & 0xffff, i);
}
/* コマンドを 1 つ動かして終了コードを返す (popen・system)。redir と file があれば "cmd > file" などにする。
   環境変数 SHELL (zosh・zsh) があれば、その行を一時ファイルに書いて "$SHELL 一時ファイル" で動かす
   (コマンドラインを通さないので、引用符・パイプ・$ もシェルの文法のまま)。無ければ COMMAND.X /C */
static int run_line(const char *prog, const char *args)
{
    static char full[256];
    struct dos_comline cl;
    char *env;
    int r, n = snprintf(cl.buffer, sizeof cl.buffer, " %s", args);
    if (n < 0 || n > 255) { errno = E2BIG; return -1; }
    cl.len = n;
    if (findprog(prog, full, sizeof full) < 0) { errno = ENOENT; return -1; }
    if (!(env = mkenv())) { errno = ENOMEM; return -1; }
    fflush(NULL);
    r = _dos_loadexec(full, &cl, env);
    free(env);
    return r < 0 ? -1 : (r & 0xff);
}
static int run_comline(const char *cmd, const char *redir, const char *file)
{
    static int seq;
    const char *sh = getenv("SHELL");
    char line[300];
    if (sh && *sh) {
        char scr[96];
        FILE *f;
        int r;
        tmpname_x(scr, sizeof scr, 's', ++seq & 7);
        if (!(f = fopen(scr, "wb"))) return -1;   /* LF のまま (CR が語の最後に残らないように) */
        if (!redir) fprintf(f, "%s\n", cmd);
        else if (redir[0] == '>' || !strchr(cmd, '|')) fprintf(f, "%s %s %s\n", cmd, redir, file);   /* パイプの最後の出力が file へ */
        else fprintf(f, "cat %s | %s\n", file, cmd);   /* "a | b < file" では a に届かないので、前に cat を付ける */
        fclose(f);
        r = run_line(sh, scr);
        unlink(scr);
        return r;
    }
    snprintf(line, sizeof line, "/C %s%s%s%s%s", cmd, redir ? " " : "", redir ? redir : "", file ? " " : "", file ? file : "");
    return run_line("command.x", line);
}
int system(const char *cmd)
{
    int r;
    if (!cmd) return 1;
    r = run_comline(cmd, NULL, NULL);
    return r < 0 ? -1 : r << 8;
}
FILE *popen(const char *cmd, const char *mode)
{
    int i;
    for (i = 0; i < NPOPEN && popens[i].f; i++) ;
    if (i == NPOPEN) { errno = EMFILE; return NULL; }
    tmpname_x(popens[i].tmp, sizeof popens[i].tmp, 'o', i);
    popens[i].w = mode[0] == 'w';
    if (popens[i].w) {
        if (!(popens[i].cmd = strdup(cmd))) return NULL;
        popens[i].f = fopen(popens[i].tmp, "w");
    } else {
        popens[i].cmd = NULL;
        if ((popens[i].st = run_comline(cmd, ">", popens[i].tmp)) < 0) return NULL;   /* 終了コードは pclose で返す */
        popens[i].f = fopen(popens[i].tmp, "r");
    }
    if (!popens[i].f) { free(popens[i].cmd); unlink(popens[i].tmp); return NULL; }
    return popens[i].f;
}
int pclose(FILE *f)
{
    int i, r;
    for (i = 0; i < NPOPEN && popens[i].f != f; i++) ;
    if (i == NPOPEN) { errno = EINVAL; return -1; }
    fclose(f);
    r = popens[i].st;
    if (popens[i].w) { r = run_comline(popens[i].cmd, "<", popens[i].tmp); free(popens[i].cmd); }
    unlink(popens[i].tmp);
    popens[i].f = NULL;
    return r < 0 ? -1 : r << 8;
}

/* rename: Human68k の _RENAME は行き先があると -22 で失敗する (XEiJ の HFS では置き換わる)。POSIX と同じく置き換える
   (patch・mv・sed -i が使う)。置き換えるのは、-22 で失敗し、元と行き先がどちらもふつうのファイルのときだけ。
   行き先はいったん同じディレクトリの別の名前にずらし、元の名前を付けられたら消す。付けられなければ行き先を戻す */
static int regfile(const char *p)
{
    struct stat st;
    return __wrap_stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static int dos2errno_rename(int r)
{
    return r == -2 || r == -3 ? ENOENT : r == -15 ? EXDEV : r == -22 ? EEXIST : r == -13 ? ENAMETOOLONG : EACCES;
}
int rename(const char *from, const char *to)
{
    static int seq;
    char tmp[256];
    const unsigned char *q;
    size_t dl = 0;
    struct dos_nameckbuf na, nb;
    int r, r2;
    /* ドライブが違えば EXDEV (mv はコピーと削除に切り替える)。行き先には何もしない */
    if (_dos_nameck(from, &na) >= 0 && _dos_nameck(to, &nb) >= 0 && toupper((unsigned char)na.drive[0]) != toupper((unsigned char)nb.drive[0])) {
        errno = EXDEV; return -1;
    }
    r = _dos_rename(from, to);
    if (r >= 0) return 0;
    if (r != -22 || !regfile(from) || !regfile(to)) { errno = dos2errno_rename(r); return -1; }
    for (q = (const unsigned char *)to; *q; q++) {   /* 行き先のディレクトリの部分 (Shift_JIS の 2 バイト目の 5C は区切りではない) */
        if (((*q >= 0x81 && *q <= 0x9f) || (*q >= 0xe0 && *q <= 0xfc)) && q[1]) { q++; continue; }
        if (*q == '/' || *q == '\\' || *q == ':') dl = q + 1 - (const unsigned char *)to;
    }
    if (dl + 9 >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, to, dl);
    snprintf(tmp + dl, sizeof tmp - dl, "rn%04x%02x", (unsigned)((unsigned long)_dos_getpdb() >> 4) & 0xffff, ++seq & 0xff);
    if ((r2 = _dos_rename(to, tmp)) < 0) { errno = dos2errno_rename(r2); return -1; }
    if ((r = _dos_rename(from, to)) < 0) {
        _dos_rename(tmp, to);   /* 元の名前を付けられなかったので、行き先を戻す */
        errno = dos2errno_rename(r);
        return -1;
    }
    _dos_delete(tmp);
    return 0;
}

/* 64 ビットの割り算 (__udivdi3 など) の速い道。68000 には 64 ビットの割り算が無く、libgcc の版は値が小さくても遅い。
   newlib の printf は %d でも 1 桁ごとに 64 ビットで /10 と %10 をするので、sprintf("%d") 1 回に約 4ms かかっていた (XEiJ)。
   両方が 32 ビットに収まるときは 32 ビットの割り算 (libgcc の __udivsi3。小さい数は divu で速い) に、割る数が 16 ビットに
   収まるときは 68000 の divu を 4 回に、それ以外は 1 ビットずつ引く */
static inline unsigned long divu_step(unsigned long n, unsigned short d)   /* n の上位 16 ビット < d のとき: 下位 16 ビット = 商、上位 = 余り */
{
    __asm__("divu.w %1,%0" : "+d"(n) : "d"(d) : "cc");
    return n;
}
static unsigned long long udivmod64(unsigned long long n, unsigned long long d, unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    int i;
    if (!(n >> 32) && !(d >> 32)) {   /* 32 ビットどうし: libgcc の __udivsi3 (小さい数を divu で割る) に任せる */
        unsigned long nn = n, dd = d, qq = nn / dd;
        if (rem) *rem = nn - qq * dd;
        return qq;
    }
    if (!(d >> 16)) {   /* 16 ビットで割る: 上から 16 ビットずつ */
        unsigned long rr = 0, x;
        int sh;
        for (sh = 48; sh >= 0; sh -= 16) {
            x = divu_step((rr << 16) | (unsigned long)((n >> sh) & 0xffff), d);
            q |= (unsigned long long)(x & 0xffff) << sh;
            rr = x >> 16;
        }
        if (rem) *rem = rr;
        return q;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
unsigned long long __udivdi3(unsigned long long n, unsigned long long d) { return udivmod64(n, d, NULL); }
unsigned long long __umoddi3(unsigned long long n, unsigned long long d) { unsigned long long r; udivmod64(n, d, &r); return r; }
long long __divdi3(long long a, long long b)
{
    int neg = (a < 0) ^ (b < 0);
    unsigned long long q = udivmod64(a < 0 ? -(unsigned long long)a : a, b < 0 ? -(unsigned long long)b : b, NULL);
    return neg ? -(long long)q : (long long)q;
}
long long __moddi3(long long a, long long b)
{
    unsigned long long r;
    udivmod64(a < 0 ? -(unsigned long long)a : a, b < 0 ? -(unsigned long long)b : b, &r);
    return a < 0 ? -(long long)r : (long long)r;
}

/* 標準出力の溜め方: newlib は stdout をはじめから「行ごとに書く」にしている (ファイルやパイプへ向けても)。
   GNU の道具は 1 行ずつ書くので、1 行ごとに DOS の _WRITE が走り、sort や uniq が 4000 行で 6〜12 秒かかっていた (XEiJ)。
   端末でなければ、main の前にまとめて書く (4KB) にする。constructor は使えない (--gc-sections で crtbegin の .init が
   消え、__INIT_SECTION__ が空になる) ので、-Wl,--wrap=main で main を包む */
int __real_main(int, char **, char **);
int __wrap_main(int argc, char **argv, char **envp)
{
    if (!isatty(1)) setvbuf(stdout, NULL, _IOFBF, 4096);
    return __real_main(argc, argv, envp);
}
