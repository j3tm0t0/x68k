/*
 * tisrun.x -- 無修正のネットワークプログラムを、同じ X68000 の上の相手 (このプログラムのスレッド) と
 * ループバック (tis.x) でつないで試す
 *
 *   tisrun.x [-o 結果ファイル] [-e 子の stderr] rcmd|ssh プログラム [引数...]
 *   結果 (PASS/FAIL の行) は -o のファイルに書く。標準出力に出すと、rcmd.x が dup2 で付け替えた
 *   ハンドル 1 を子の終了時に閉じられて消える
 *
 *   tisrun.x rcmd RCMD.X [引数...]   rcmd.x をフォアグラウンドで動かし、スレッドがクライアントになる:
 *                                    echo / type BIG.TXT (約 300KB を受信) / @put (300KB を送信) / exit
 *   tisrun.x ssh SSH.X [引数...]     ssh.x をフォアグラウンドで動かし、スレッドが 127.0.0.1:22 の偽の
 *                                    SSH サーバになる (バナーを送り、相手のバナーと KEXINIT を受け取って切る)
 *
 * Human68k はシングルタスクだが、PROCESS 指定があれば _OPEN_PR でスレッドを作れる。フォアグラウンド
 * (DOS _EXEC で起動した子) が _CHANGE_PR で譲るか (rcmd.x の待ちや libsocket の poll)、タイムスライスで
 * 切り替わったときにスレッドが動く。スレッドはこのプログラムのメモリ (コードとスタック) で動くので、
 * 子が終わったらスレッドの終わりを待ってから終了する。
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <x68k/dos.h>

#define BIG_LINES 5000                  /* BIG.TXT の行数 (1 行 60 バイト = 300,000 バイト) */
#define PUT_SIZE  300000

static char logbuf[16384];
static int loglen;
static volatile int thread_done, child_done, npass, nfail;
static int ssh_stop;                    /* 2 = 偽の SSH サーバが KEXINIT まで受け取った */

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  if (loglen < (int)sizeof logbuf - 200)
    loglen += vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap);
  va_end(ap);
}

static void check(int ok, const char *what)
{
  lg("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (ok) npass++; else nfail++;
}

static void big_line(char *p, int i)
{
  sprintf(p, "line %06d abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRS\r\n", i);   /* 60 バイト */
}

static unsigned char put_byte(long i)
{
  return (unsigned char)((i * 7 + (i >> 8)) ^ 0x5a);
}

static void wait_ms(int ms)
{
  long t0 = _dos_time_pr();
  while (_dos_time_pr() - t0 < ms)
    _dos_change_pr();
}

static int conn(int port, int tries)
{
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  while (tries-- > 0 && !child_done) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
      return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0)
      return s;
    close(s);
    wait_ms(200);
  }
  return -1;
}

static int send_all(int s, const void *p, long n)
{
  const char *q = p;
  while (n > 0) {
    int k = write(s, q, n > 8192 ? 8192 : n);
    if (k <= 0)
      return -1;
    q += k;
    n -= k;
  }
  return 0;
}

/* EOF まで (か ms ミリ秒何も来ないまで) 受け取り、cb に渡す。戻り値は受け取ったバイト数 */
static long recv_until_eof(int s, int ms, void (*cb)(const char *, int))
{
  static char buf[4096];
  struct pollfd p;
  long total = 0, t0 = _dos_time_pr();
  p.fd = s;
  p.events = POLLIN;
  for (;;) {
    int r = poll(&p, 1, 200), n;
    if (r == 0) {
      if (_dos_time_pr() - t0 > ms)
        break;
      continue;
    }
    n = read(s, buf, sizeof buf);
    if (n <= 0)
      break;
    cb(buf, n);
    total += n;
    if (ssh_stop == 2)
      break;
    t0 = _dos_time_pr();
  }
  return total;
}

/* ---- rcmd のクライアント ---- */

static char tail[256];
static int taillen;
static long bigpos, bigbad = -1;
static int bigmode;
static char resp[2048];
static int resplen;

/* 受け取った物の最後 256 バイト (終わりの印を見る) と、先頭 2KB を残す。bigmode なら BIG.TXT と比べる */
static void rcmd_cb(const char *p, int n)
{
  int k;
  for (k = 0; k < n; k++) {
    if (resplen < (int)sizeof resp - 1)
      resp[resplen++] = p[k];
    if (taillen == sizeof tail)
      memmove(tail, tail + 1, --taillen);
    tail[taillen++] = p[k];
    if (bigmode && bigpos < (long)BIG_LINES * 60) {
      char line[64];
      big_line(line, bigpos / 60);
      if (line[bigpos % 60] != p[k] && bigbad < 0)
        bigbad = bigpos;
      bigpos++;
    }
  }
}

static int rcmd_cmd(const char *cmd, const void *data, long dlen, int ms)
{
  int s = conn(2323, 100);
  char line[300];
  long t0, n;
  if (s < 0) {
    lg("connect 127.0.0.1:2323 failed\n");
    return -1;
  }
  taillen = resplen = 0;
  bigpos = 0;
  bigbad = -1;
  sprintf(line, "%s\r\n", cmd);
  t0 = _dos_time_pr();
  if (send_all(s, line, strlen(line)) < 0 || (data && send_all(s, data, dlen) < 0)) {
    lg("send failed\n");
    close(s);
    return -1;
  }
  n = recv_until_eof(s, ms, rcmd_cb);
  close(s);
  resp[resplen] = 0;
  tail[taillen < (int)sizeof tail ? taillen : (int)sizeof tail - 1] = 0;
  lg("[%s] -> %ld bytes in %ld ms\n", cmd, n, _dos_time_pr() - t0);
  return n;
}

static void rcmd_thread(void)
{
  static char put[PUT_SIZE];   /* malloc は使えない (子がすぐ後ろのメモリを取るのでヒープを伸ばせない) */
  long n;
  wait_ms(500);
  /* 1. 小さいコマンド (XEiJ のホストファイルシステムから起動すると、dup2 で付け替えた標準出力に子の出力が
     入らない (tests/README 参照、tis.x とは無関係) ので、出力の中身は見ず、終わりの印だけ確かめる) */
  n = rcmd_cmd("echo hello", 0, 0, 30000);
  check(n > 0 && strstr(tail, "--rcmd-end rc=0--"), "rcmd: echo hello (end mark)");
  /* 2. (300KB の受信は、上の理由で rcmd.out に中身を入れられないので省く。tistest.x が両方向を見る) */
  /* 3. 300KB を送る (@put)。中身はスレッドが終わったあと main が確かめる */
  {
    char cmd[80];
    long i;
    for (i = 0; i < PUT_SIZE; i++)
      put[i] = put_byte(i);
    sprintf(cmd, "@put A:\\TMP\\PUT.BIN %d", PUT_SIZE);
    n = rcmd_cmd(cmd, put, PUT_SIZE, 30000);
    check(n > 0 && strstr(tail, "--rcmd-end rc=0--"), "rcmd: @put 300000 acknowledged");
  }
  /* 4. 終わらせる。返事はブロッキングの read で受ける (データが来るまで tis.x の中で待ち、EOF で 0) */
  {
    int s = conn(2323, 100), k = -1;
    long t0 = _dos_time_pr(), waits0 = 0;
    resplen = 0;
    if (s >= 0 && send_all(s, "exit\r\n", 6) == 0) {
      while (resplen < (int)sizeof resp - 1 && (k = read(s, resp + resplen, sizeof resp - 1 - resplen)) > 0)
        resplen += k, waits0++;
      resp[resplen] = 0;
      lg("[exit] blocking reads: %ld calls, %d bytes, last %d, %ld ms\n", waits0, resplen, k, _dos_time_pr() - t0);
      check(strstr(resp, "bye") && strstr(resp, "--rcmd-end rc=0--") && k == 0, "rcmd: exit -> bye (blocking read until EOF)");
    } else
      check(0, "rcmd: exit");
    if (s >= 0)
      close(s);
  }
}

/* ---- 偽の SSH サーバ ---- */

static void ssh_cb(const char *p, int n)
{
  int k;
  char *e;
  for (k = 0; k < n && resplen < (int)sizeof resp - 1; k++)
    resp[resplen++] = p[k];
  resp[resplen] = 0;
  /* バナー行の後ろに 4 バイトの長さ分そろったら終わり */
  if (ssh_stop && (e = memchr(resp, '\n', resplen)) != 0) {
    unsigned char *q = (unsigned char *)e + 1;
    int rest = resplen - (q - (unsigned char *)resp);
    if (rest >= 4 && rest >= 4 + (int)((q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]))
      ssh_stop = 2;
  }
}

static void ssh_thread(void)
{
  struct sockaddr_in a;
  struct pollfd p;
  int ls = socket(AF_INET, SOCK_STREAM, 0), s, r;
  long t0;
  const char *banner = "SSH-2.0-tisfake_0.1\r\n";
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(22);
  if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
    check(0, "ssh: listen 22");
    return;
  }
  p.fd = ls;
  p.events = POLLIN;
  r = poll(&p, 1, 60000);
  if (r != 1 || (s = accept(ls, 0, 0)) < 0) {
    check(0, "ssh: accept");
    close(ls);
    return;
  }
  close(ls);
  check(1, "ssh: ssh.x connected to 127.0.0.1:22");
  t0 = _dos_time_pr();
  send_all(s, banner, strlen(banner));
  resplen = 0;
  /* 相手のバナーと、続く KEXINIT (二進のパケット) を受け取る。ssh.x は connect のあと準備に何秒もかかる
     (10MHz の 68000) ので、KEXINIT らしい量が来るか 60 秒何も来なくなるまで待つ */
  ssh_stop = 1;
  recv_until_eof(s, 60000, ssh_cb);
  close(s);
  resp[resplen < (int)sizeof resp ? resplen : (int)sizeof resp - 1] = 0;
  {
    char *e = strchr(resp, '\n'), m[160];
    int bl = e ? e - resp + 1 : 0;
    sprintf(m, "ssh: client banner [%.*s], then %d more bytes (KEXINIT) in %ld ms", bl > 2 ? bl - 2 : 0, resp,
            resplen - bl, _dos_time_pr() - t0);
    check(!strncmp(resp, "SSH-2.0-", 8) && resplen - bl > 100 && (unsigned char)resp[bl + 5] == 20, m);
  }
}

/* ---- スレッド ---- */

static char mode[8];
static char ustack[32768] __attribute__((aligned(4)));
static char sstack[8192] __attribute__((aligned(4)));
static struct dos_prcctrl prc;
static unsigned char mbox[4];

static void thread_main(void)
{
  if (!strcmp(mode, "rcmd"))
    rcmd_thread();
  else
    ssh_thread();
  thread_done = 1;
  for (;;)
    _dos_kill_pr();
}

static int make_big(void)
{
  FILE *f = fopen("A:\\BIG.TXT", "wb");
  char line[64];
  int i;
  if (!f)
    return -1;
  for (i = 0; i < BIG_LINES; i++) {
    big_line(line, i);
    fwrite(line, 1, 60, f);
  }
  fclose(f);
  return 0;
}

static int check_put(void)
{
  FILE *f = fopen("A:\\TMP\\PUT.BIN", "rb");
  long i = 0;
  int c, bad = 0;
  if (!f)
    return -1;
  while ((c = getc(f)) != EOF) {
    if ((unsigned char)c != put_byte(i))
      bad++;
    i++;
  }
  fclose(f);
  return i == PUT_SIZE && !bad ? 0 : -2;
}

int main(int argc, char **argv)
{
  struct dos_comline cl;
  int tid, rc, i;
  long t0;
  static struct dos_prcptr me;
  static char msg[4];

  const char *outf = 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *errf = 0;
  for (;;) {
    if (argc > 2 && !strcmp(argv[1], "-o"))
      outf = argv[2];
    else if (argc > 2 && !strcmp(argv[1], "-e"))
      errf = argv[2];                   /* 子の標準エラー出力 (ハンドル 2) をこのファイルへ */
    else
      break;
    argv += 2;
    argc -= 2;
  }
  if (argc < 3 || (strcmp(argv[1], "rcmd") && strcmp(argv[1], "ssh"))) {
    fprintf(stderr, "usage: tisrun.x [-o file] [-e file] rcmd|ssh program [args...]\n");
    return 1;
  }
  strcpy(mode, argv[1]);
  if (!strcmp(mode, "rcmd") && make_big() < 0) {
    printf("FAIL cannot write A:\\BIG.TXT\n");
    return 1;
  }
  /* 子のコマンド行 (先頭 1 バイトが長さ) */
  cl.len = 0;
  for (i = 3; i < argc; i++) {
    int l = strlen(argv[i]);
    if (cl.len + l + 1 >= 255)
      break;
    cl.buffer[cl.len++] = ' ';
    memcpy(cl.buffer + cl.len, argv[i], l);
    cl.len += l;
  }
  cl.buffer[cl.len] = 0;

  prc.length = sizeof mbox;
  prc.buf_ptr = mbox;
  prc.your_id = 0xffff;
  tid = _dos_open_pr("tisrun", 2, (int)(ustack + sizeof ustack), (int)(sstack + sizeof sstack), 0x0000,
                     (int)thread_main, &prc, 0);
  if (tid < 0) {
    printf("FAIL _OPEN_PR %d (CONFIG.SYS の PROCESS を確認)\n", tid);
    return 1;
  }
  {
    /* 眠った状態で作られるので 1 通送って起こす (npd.x と同じ) */
    int myid = _dos_get_pr(-1, &me), r;
    while ((r = _dos_send_pr(myid, tid, 0, msg, sizeof msg)) == -28)
      _dos_change_pr();
  }
  printf("tisrun: thread %d (%s), exec %s%s\n", tid, mode, argv[2], cl.buffer);
  {
    int ef = -1, save = -1;
    if (errf && (ef = _dos_create(errf, 0x20)) >= 0) {
      save = _dos_dup(2);
      _dos_dup2(ef, 2);
    }
    rc = _dos_loadexec(argv[2], &cl, 0);
    if (ef >= 0) {
      _dos_dup2(save, 2);
      _dos_close(save);
      _dos_close(ef);
    }
  }
  child_done = 1;
  lg("tisrun: %s exited, rc=%d\n", argv[2], rc);
  t0 = _dos_time_pr();
  while (!thread_done && _dos_time_pr() - t0 < 20000)
    _dos_change_pr();
  if (!thread_done) {
    _dos_suspend_pr(tid);
    check(0, "thread did not finish (suspended)");
  }
  if (!strcmp(mode, "rcmd")) {
    rc = check_put();
    check(rc == 0, "rcmd: A:\\TMP\\PUT.BIN content (300000 bytes)");
  }
  lg("done: %d pass, %d fail\n", npass, nfail);
  {
    FILE *f = outf ? fopen(outf, "w") : stdout;
    if (f) {
      fwrite(logbuf, 1, loglen, f);
      if (outf)
        fclose(f);
    }
  }
  if (!thread_done)
    for (;;)                            /* スレッドがこのメモリで動いているので終われない */
      _dos_change_pr();
  return nfail ? 1 : 0;
}
