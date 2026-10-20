/*
 * rcmd.x -- X68000 remote command server (ESP/X socket library)
 *
 * TCP で接続を受け、1 行受け取ったコマンドを zsh.x -c で実行し、
 * 標準出力・標準エラー出力をそのまま送り返す。1 接続 1 コマンド。
 * 出力の最後に "--rcmd-end rc=<終了コード>--" の行を付ける。
 * "exit" を受け取るか、X68000 側で ESC キーを押すとサーバを終了する。
 * "@put <path> <size>" の行に続けて size バイト送ると、path にそのまま書き込む (x68put)。
 * "@time YYYY MM DD hh mm ss" で X68000 の日付・時刻を設定する (Raspberry Pi 側が PPP 接続時に送る)。
 * "@restart" を受け取ると $temp/rcmd.rst を作って終わる。zsh の rcmd 関数はこのファイルを見て rcmd.x を起動し直す
 * (X68k 版 zsh 2.5 では終了コード 100 を $? で受け取れなかったので、ファイルで知らせる)
 * (Human68k に exec が無いので、新しい rcmd.x を make install したあとの入れ替えに使う)。
 *
 * Human68k はフォアグラウンドが DOS コールで待っている間 (accept / recvline のブロック中や
 * シェルのキー入力待ち) バックグラウンドスレッド (xip.x, ppp.x) を動かさないので、
 * 受信パケットが処理されず接続できない。そのためブロックする呼び出しを避け、
 * sockstate() / socklen() をポーリングしながら _dos_change_pr() で CPU を譲る。
 *
 *   usage: rcmd.x [-p port] [-s shell] [-d] [-v] [-B]   (default: -p 2323 -s zsh.x)
 *          -d: 接続ごとの状態を表示  -v: 実行したコマンドの出力を X68000 の画面にも表示
 *   build: gcc rcmd.c -o rcmd.x -lnetwork   (xip.x / inetd.x 常駐中に実行する)
 *
 * Raspberry Pi 側: templates/raspi/x68.sh 'ls -l'
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <doslib.h>
#include <iocslib.h>
#include "network.h"
#include "socket.h"

#define END_MARK "--rcmd-end rc=%d--\n"

static char tmpout[128];

/* sockstate() は "LISTEN" "SYN RECEIVED" "ESTABLISHED" のように大文字で返す (X68000 Z 実機で確認)。
   念のため大文字小文字を区別せずに先頭を比べる */
/* sockstate() は、相手に切られたソケットなどでは文字列ではない値 (エラー番号らしきもの) を返す。
   そのまま読むとバスエラーになったので、RAM の範囲のアドレスでなければ "CLOSED" とみなす */
static const char *safe_state(int sock)
{
  const char *st = sockstate(sock);
  unsigned long a = (unsigned long)st;
  if (a < 0x1000 || a >= 0xc00000)
    return "CLOSED";
  return st;
}

static int state_is(int sock, const char *want)
{
  const char *st = safe_state(sock);
  while (*want) {
    int a = (unsigned char)*st++, b = (unsigned char)*want++;
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b)
      return 0;
  }
  return 1;
}

/* port で待ち受けるソケットを作る。listen の backlog は 0 (複製しない) にして、
   接続が来たら listen 用ソケット自体が Established になるのを sockstate() で検知する
   (backlog 1 以上だと複製側のソケットが accept() 前には見えず、ブロックせずに検知できない) */
static int open_listener(int port)
{
  struct sockaddr_in sin;
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0)
    return -1;
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = port;
  sin.sin_addr.s_addr = 0;
  if (bind(ls, (char *)&sin, sizeof sin) < 0) {
    fprintf(stderr, "rcmd: bind failed: %s\n", sockerr(ls));
    close_s(ls);
    return -1;
  }
  listen(ls, 0);   /* backlog 0: 接続が来ると listen ソケット自身が Established になる */
  return ls;
}

/* cmd を shell -c で実行し、出力を tmpout に落とす。戻り値は終了コード */
static int run_command(char *shell, char *cmd)
{
  int fd, so, se, rc;

  fd = open(tmpout, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return -1;
  so = dup(1);
  se = dup(2);
  dup2(fd, 1);
  dup2(fd, 2);
  close(fd);
  rc = spawnlp(P_WAIT, shell, shell, "-c", cmd, (char *)0);
  dup2(so, 1);
  dup2(se, 2);
  close(so);
  close(se);
  return rc;
}

/* ソケットへ len バイト書く (write_s は途中で返ることがある) */
static int send_all(int s, char *p, int len)
{
  while (len > 0) {
    int n = write_s(s, p, len);
    if (n <= 0)
      return -1;
    p += n;
    len -= n;
  }
  return 0;
}

/* path の中身をソケットへ送る。戻り値は送ったバイト数、*cs に所要時間 (1/100 秒) */
static long send_file(int s, char *path, int *cs)
{
  static char buf[2048];
  int fd, n, t0 = _iocs_ontime();
  long total = 0;

  *cs = 0;
  fd = open(path, O_RDONLY | O_BINARY);
  if (fd < 0)
    return 0;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    if (send_all(s, buf, n) < 0)
      break;
    total += n;
  }
  close(fd);
  *cs = _iocs_ontime() - t0;
  if (*cs < 0)
    *cs += 24 * 60 * 60 * 100;
  return total;
}

/* "@time Y M D h m s" : DOS _SETDATE / _SETTIM2 で時計を合わせる */
static int set_clock(char *args)
{
  int y, mo, d, h, mi, se;
  if (sscanf(args, "%d %d %d %d %d %d", &y, &mo, &d, &h, &mi, &se) != 6 || y < 1980)
    return 2;
  if (_dos_setdate(((y - 1980) << 9) | (mo << 5) | d) < 0)
    return 3;
  if (_dos_settim2((h << 16) | (mi << 8) | se) < 0)
    return 4;
  return 0;
}

/* "@put <path> <size>" : 続く size バイトを受信して path に書く。戻り値は終了コード */
static int receive_file(int s, char *args, int debug)
{
  static char buf[2048];
  char path[256];
  long size, got = 0;
  int fd, t0, el, start;

  if (sscanf(args, "%255s %ld", path, &size) != 2 || size < 0)
    return 2;
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
  if (fd < 0)
    return 3;
  t0 = start = _iocs_ontime();
  while (got < size) {
    int n = socklen(s, 0);
    if (n > 0) {
      if (n > (int)sizeof buf) n = sizeof buf;
      if (n > size - got) n = size - got;
      n = read_s(s, buf, n);
      if (n <= 0)
        break;
      write(fd, buf, n);
      got += n;
      t0 = _iocs_ontime();
      continue;
    }
    if (n < 0)
      break;
    el = _iocs_ontime() - t0;
    if (el < 0)
      el += 24 * 60 * 60 * 100;
    if (el > 3000)           /* 30 秒何も来なければ諦める */
      break;
    _dos_change_pr();
  }
  close(fd);
  el = _iocs_ontime() - start;   /* 1/100 秒 */
  if (el < 0)
    el += 24 * 60 * 60 * 100;
  if (el < 1)
    el = 1;
  /* 転送ログは常に出す (debug でなくても) */
  {
    long bps = got * 800 / el;   /* bit/s (el は 1/100 秒) */
    printf("rcmd: put %s %ld/%ld bytes in %d.%02ds (%ld B/s = %ld.%ld kbps)\n", path, got, size,
           el / 100, el % 100, got * 100 / el, bps / 1000, bps % 1000 / 100);
  }
  (void)debug;
  return got == size ? 0 : 4;
}

int main(int argc, char **argv)
{
  int port = 2323, debug = 0, poll = 1, verbose = 0;
  char *shell = "zsh.x";
  char laststate[32];
  char *tmp;
  int ls, i;
  static char line[1024], mark[64];

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-p") && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-s") && i + 1 < argc)
      shell = argv[++i];
    else if (!strcmp(argv[i], "-d"))
      debug = 1;
    else if (!strcmp(argv[i], "-v"))
      verbose = 1;
    else if (!strcmp(argv[i], "-B"))
      poll = 0;   /* accept() でブロックして待つ (ESC は効かない) */
    else {
      fprintf(stderr, "usage: rcmd.x [-p port] [-s shell] [-d] [-v] [-B]\n");
      return 1;
    }
  }
  tmp = getenv("temp");
  if (!tmp)
    tmp = "A:\\tmp";
  sprintf(tmpout, "%s\\rcmd.out", tmp);

  ls = open_listener(port);
  if (ls < 0) {
    fprintf(stderr, "rcmd: socket failed (xip.x / inetd.x is not running?)\n");
    return 1;
  }
  printf("rcmd: listening on port %d, shell %s (remote \"exit\" or ESC stops)\n", port, shell);

  for (;;) {
    struct sockaddr_in peer;
    int plen = sizeof peer;
    int c, n, rc;
    unsigned char *a;

    /* 接続待ち (既定): accept() でブロックせず listen ソケットの状態を見て待ち、ESC で終了できる。
       backlog 0 なので接続が来ると listen ソケット自身が SYN RECEIVED / Established になる。
       -B なら accept() でブロックする (X68000 Z ではブロック中も xip/ppp のスレッドが動く) */
    if (poll) {
      laststate[0] = '\0';
      for (;;) {
        const char *st = safe_state(ls);
        if (debug && strncmp(st, laststate, sizeof laststate - 1)) {
          strncpy(laststate, st, sizeof laststate - 1);
          printf("rcmd: state [%s]\n", st);
        }
        if (!state_is(ls, "LISTEN"))
          break;
        if (_iocs_b_keysns() && (_iocs_b_keyinp() & 0xff) == 0x1b) {
          printf("rcmd: ESC, stop\n");
          close_s(ls);
          return 0;
        }
        _dos_change_pr();
      }
    }
    c = accept(ls, (char *)&peer, &plen);
    if (debug)
      printf("rcmd: accept -> %d (listen state [%s])\n", c, safe_state(ls));
    if (c < 0) {
      fprintf(stderr, "rcmd: accept failed: %s\n", sockerr(ls));
      break;
    }
    sockmode(c, SOCK_ASCII);
    /* accept() は SYN RECEIVED のまま戻ることがあるので、接続が確立してコマンド行が
       届くまで CPU を譲りながら待つ (10 秒で諦める。ontime は 1/100 秒) */
    {
      int t0 = _iocs_ontime();
      for (;;) {
        const char *st = safe_state(c);
        int el = _iocs_ontime() - t0;
        if (el < 0)
          el += 24 * 60 * 60 * 100;
        if (state_is(c, "ESTABLISHED") && socklen(c, 0) > 0)
          break;
        if (el > 1000 || !(state_is(c, "SYN") || state_is(c, "ESTABLISHED"))) {
          if (debug)
            printf("rcmd: give up [%s] after %d.%02d sec\n", st, el / 100, el % 100);
          break;
        }
        _dos_change_pr();
      }
      if (debug)
        printf("rcmd: state [%s] len=%d\n", safe_state(c), socklen(c, 0));
    }
    n = recvline(c, line, sizeof line - 1);
    if (debug)
      printf("rcmd: recvline -> %d\n", n);
    if (n <= 0) {
      close_s(c);
      if (c != ls) close_s(ls);
      if ((ls = open_listener(port)) < 0) break;
      continue;
    }
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    a = (unsigned char *)&peer.sin_addr.s_addr;
    printf("rcmd: [%d.%d.%d.%d] %s\n", a[0], a[1], a[2], a[3], line);
    sockmode(c, SOCK_BINARY);
    if (!strcmp(line, "exit") || !strcmp(line, "@restart")) {
      int restart = line[0] == '@';
      send_all(c, restart ? "restarting\n" : "bye\n", restart ? 11 : 4);
      sprintf(mark, END_MARK, 0);
      send_all(c, mark, strlen(mark));
      usflush(c);
      close_s(c);
      if (c != ls)
        close_s(ls);
      if (restart) {
        char flag[160];
        int fd;
        sprintf(flag, "%s\\rcmd.rst", getenv("temp") ? getenv("temp") : "A:\\tmp");
        if ((fd = open(flag, O_WRONLY | O_CREAT | O_TRUNC, 0666)) >= 0)
          close(fd);
      }
      return 0;
    }
    if (!strncmp(line, "@put ", 5))
      rc = receive_file(c, line + 5, debug);
    else if (!strncmp(line, "@time ", 6))
      rc = set_clock(line + 6);
    else
      rc = run_command(shell, line);
    if (debug)
      printf("rcmd: rc=%d, sending %s\n", rc, tmpout);
    if (line[0] != '@') {
      int cs;
      long sent = send_file(c, tmpout, &cs);
      if (sent >= 1024 || debug) {   /* ある程度の量を返したときは送信速度も出す */
        long bps = cs > 0 ? sent * 800 / cs : 0L;   /* bit/s */
        printf("rcmd: sent %ld bytes in %d.%02ds (%ld B/s = %ld.%ld kbps)\n", sent, cs / 100, cs % 100,
               cs > 0 ? sent * 100 / cs : 0L, bps / 1000, bps % 1000 / 100);
      }
      if (verbose) {   /* 同じ出力を画面にも出す (先頭 2KB まで。バイナリで画面が壊れないよう制御文字は '.') */
        static char vb[2048];
        int fd = open(tmpout, O_RDONLY | O_BINARY), k, j;
        if (fd >= 0) {
          k = read(fd, vb, sizeof vb);
          close(fd);
          for (j = 0; j < k; j++) {
            unsigned char ch = vb[j];
            if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t')
              vb[j] = '.';
            else if (ch == 0x7f)
              vb[j] = '.';
          }
          if (k > 0)
            write(1, vb, k);
          if (sent > (long)sizeof vb)
            printf("\nrcmd: (画面には先頭 %d / %ld バイトだけ表示)\n", (int)sizeof vb, sent);
        }
        printf("rcmd: rc=%d\n", rc);
      }
    }
    sprintf(mark, END_MARK, rc);
    send_all(c, mark, strlen(mark));
    usflush(c);
    close_s(c);
    if (c != ls)
      close_s(ls);
    /* 次の接続用に listen し直す */
    if ((ls = open_listener(port)) < 0) {
      fprintf(stderr, "rcmd: cannot re-listen\n");
      break;
    }
  }
  close_s(ls);
  return 0;
}
