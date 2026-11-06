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
char *ttyname(int fd) { return isatty(fd) ? "/dev/con" : NULL; }
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
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) { (void)n; (void)r; (void)w; (void)e; (void)t; errno = ENOSYS; return -1; }

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
    for (p = path; ; ) {
        const char *e = p;
        while (*e && *e != ';') e++;
        for (i = 0; ext[i]; i++) {
            if (e == p) snprintf(out, n, "%s%s", name, ext[i]);
            else snprintf(out, n, "%.*s%s%s%s", (int)(e - p), p, (e[-1] == '/' || e[-1] == '\\') ? "" : "/", name, ext[i]);
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
int __real_stat(const char *, struct stat *);
int __real_fstat(int, struct stat *);
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
    int r = __real_stat(path, st);
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
    r = __real_fstat(fd, st);
    if (r == 0) setino(fd >= 0 && fd < 64 ? __fd_list[fd].name : NULL, st, fd);
    return r;
}

/* close: 標準入出力 (0〜4) は親 (シェル) と同じ DOS の番号なので、子が閉じると親の出力まで閉じる。
   GNU の道具は終わるときに close_stdout で標準出力を閉じるので、0〜4 は閉じたことにして実際には閉じない
   (-Wl,--wrap=close。終了時に Human68k が後始末する) */
int __real_close(int);
static int dirfd_close(int fd);
int __wrap_close(int fd)
{
    if (fd >= 0 && fd <= 4) return 0;
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
    if (dirfd_path(fd)) return 1;
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
static int chdir_drive(const char *p)
{
    if (p[0] && p[1] == ':') _dos_chgdrv(toupper((unsigned char)p[0]) - 'A');
    return chdir(p);
}
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    int mode, r, e;
    struct stat st;
    va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
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
    if (k && base[k - 1] != '\\' && base[k - 1] != '/') out[k++] = '\\';
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
        dirmap_add(d, -1, abs[0] ? abs : NULL);
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
int __wrap_chdir(const char *p)
{
    if (!start_saved) {
        start_saved = 1;
        if (!__real_getcwd(start_cwd, sizeof start_cwd)) start_cwd[0] = 0;
    }
    return __real_chdir(p);
}
void __real_exit(int) __attribute__((noreturn));
void __wrap_exit(int st)
{
    if (start_saved && start_cwd[0]) {
        start_saved = 0;
        if (start_cwd[1] == ':') _dos_chgdrv(toupper((unsigned char)start_cwd[0]) - 'A');
        __real_chdir(start_cwd);
    }
    __real_exit(st);
}

/* 仮のディレクトリの番号を read したら EISDIR (cat DIR が "Is a directory" と出す) (-Wl,--wrap=read) */
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *b, size_t n)
{
    if (dirfd_path(fd)) { errno = EISDIR; return -1; }
    return __real_read(fd, b, n);
}
