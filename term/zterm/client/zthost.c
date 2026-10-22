/*
 * zthost.c -- Mac で zterm の Z 側を動かす試験用クライアント (ssh.x の代わり)。
 * 起動スクリプトを引数の末尾に付けてコマンドを起動し (例: ssh raspi5 / sh -c)、その標準入出力で
 * ztclient.c の手順 (ヘルパ転送・フレーム・ACK) を回す。描画は vt100.c のホスト版 VRAM (-DVT_HOSTTEST)。
 *
 * usage: zthost [-d DIST] [-g CGROM] [-v VRAM.bin] [-t TEXT] [-k KEYS] [-D MS] [-V] [-q] -- cmd args...
 *   -d DIST   ヘルパ (<arch>.gz) と VERSION の置き場 (既定 ../dist)
 *   -g CGROM  実フォント (term/vt100/tests/cgrom/CGROM.DAT) で描く
 *   -v FILE   終了時の VRAM (tests/vram2png.py 形式)。-t は論理画面の文字
 *   -k FILE   キー台本: 1 行 1 命令。"W 秒" 待つ / "K 文字列" 打鍵 (\r \n \e \t \xNN) / "S 名前" その時点の
 *             VRAM と文字を 名前.bin / 名前.txt に / "F" 再描画要求 / "Q" 標準入力を閉じる
 *   -D MS     1 フレーム描くごとに MS ミリ秒止まる (遅い Z の真似。ACK が遅れサーバがフレームをまとめる)
 *   -V        フレームごとに VRAM = 論理画面の描き直し か確かめる (差分描画の試験)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "ztclient.h"
#include "vt100.h"

extern unsigned char vt_host_vram[], vt_host_cgrom[];
extern unsigned short vt_host_pal[];
int vt_host_verify(void);
void vt_dump_text(FILE *f, int cols, int rows);

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
static int quiet;
static void logline(const char *m) { if (!quiet) fprintf(stderr, "[zthost %.2f] %s\n", now(), m); }

static void dump(const char *vram, const char *text)
{
  int c, r;
  vt_get_size(&c, &r);
  if (vram) {
    FILE *f = fopen(vram, "wb");
    int i;
    if (f) {
      for (i = 0; i < 4; i++) fwrite(vt_host_vram + (long)i * 0x20000, 1, 512 * 128, f);
      for (i = 0; i < 16; i++) { fputc(vt_host_pal[i] >> 8, f); fputc(vt_host_pal[i] & 0xff, f); }
      fclose(f);
    }
  }
  if (text) { FILE *f = fopen(text, "w"); if (f) { vt_dump_text(f, c, r); fclose(f); } }
}

static int unesc(const char *s, unsigned char *o)
{
  int n = 0;
  while (*s) {
    if (*s == '\\' && s[1]) {
      s++;
      switch (*s) {
        case 'r': o[n++] = '\r'; break;
        case 'n': o[n++] = '\n'; break;
        case 'e': o[n++] = 0x1b; break;
        case 't': o[n++] = '\t'; break;
        case 'x': { unsigned v = 0; sscanf(s + 1, "%2x", &v); o[n++] = (unsigned char)v; s += 2; break; }
        default: o[n++] = (unsigned char)*s; break;
      }
      s++;
    } else o[n++] = (unsigned char)*s++;
  }
  return n;
}

int main(int argc, char **argv)
{
  const char *dist = "../dist", *vram = 0, *text = 0, *keys = 0;
  int delay = 0, verify = 0, i, to[2], from[2], cols = 96, rows = 32, nbad = 0;
  pid_t pid;
  FILE *kf = 0;
  double t0, kwait = 0;
  long kbytes = 0;
  char **cmd;
  int ncmd, in_open = 1;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "-d") && i + 1 < argc) dist = argv[++i];
    else if (!strcmp(argv[i], "-v") && i + 1 < argc) vram = argv[++i];
    else if (!strcmp(argv[i], "-t") && i + 1 < argc) text = argv[++i];
    else if (!strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
    else if (!strcmp(argv[i], "-D") && i + 1 < argc) delay = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &cols, &rows);
    else if (!strcmp(argv[i], "-V")) verify = 1;
    else if (!strcmp(argv[i], "-q")) quiet = 1;
    else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
      FILE *g = fopen(argv[++i], "rb");
      if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cannot read CGROM %s\n", argv[i]); return 1; }
      fclose(g);
    } else { fprintf(stderr, "zthost: unknown option %s\n", argv[i]); return 2; }
  }
  if (i >= argc) { fprintf(stderr, "usage: zthost [opts] -- cmd args...\n"); return 2; }
  zc_init(dist, cols, rows);
  zc_log(logline);
  ncmd = argc - i;
  cmd = (char **)calloc(ncmd + 2, sizeof *cmd);
  memcpy(cmd, argv + i, ncmd * sizeof *cmd);
  cmd[ncmd] = (char *)zc_command();
  if (keys && !(kf = fopen(keys, "r"))) { fprintf(stderr, "cannot open %s\n", keys); return 1; }
  signal(SIGPIPE, SIG_IGN);
  if (pipe(to) || pipe(from)) return 1;
  pid = fork();
  if (pid == 0) {
    dup2(to[0], 0); dup2(from[1], 1);
    close(to[0]); close(to[1]); close(from[0]); close(from[1]);
    execvp(cmd[0], cmd);
    perror("exec"); _exit(127);
  }
  close(to[0]); close(from[1]);
  t0 = now();
  for (;;) {
    struct pollfd pf[2];
    unsigned char buf[65536];
    int n, np = 1, tmo = 100;
    int s = zc_state();
    if (s == ZC_DONE || s == ZC_FAIL) break;
    /* キー台本 */
    while (kf && now() >= kwait) {
      char l[512];
      if (!fgets(l, sizeof l, kf)) { fclose(kf); kf = 0; break; }
      l[strcspn(l, "\n")] = 0;
      if (l[0] == 'W') kwait = now() + atof(l + 1);
      else if (l[0] == 'K') { unsigned char k[512]; int m = unesc(l + 2, k); zc_keys(k, m); kbytes += m; }
      else if (l[0] == 'S') { char a[300], b[300]; sprintf(a, "%s.bin", l + 2); sprintf(b, "%s.txt", l + 2); dump(a, b); logline(l); }
      else if (l[0] == 'F') zc_refresh();
      else if (l[0] == 'Q') { close(to[1]); in_open = 0; }
    }
    pf[0].fd = from[0]; pf[0].events = POLLIN;
    if (in_open && zc_pending() > 0) { pf[1].fd = to[1]; pf[1].events = POLLOUT; np = 2; }
    if (poll(pf, np, tmo) < 0) break;
    if (pf[0].revents & (POLLIN | POLLHUP)) {
      n = (int)read(from[0], buf, sizeof buf);
      if (n <= 0) { logline("remote closed"); break; }
      {
        long f0, f1;
        zc_stats(0, &f0, 0);
        zc_input(buf, n);
        zc_stats(0, &f1, 0);
        if (f1 != f0) {
          if (verify && zc_started() && !vt_host_verify()) { nbad++; if (nbad < 5) logline("VERIFY FAILED"); }
          if (delay) usleep(delay * 1000 * (f1 - f0));
        }
      }
    }
    if (np == 2 && (pf[1].revents & POLLOUT)) {
      n = zc_output(buf, 16384);
      if (n > 0 && write(to[1], buf, n) != n) { logline("write failed"); break; }
    }
  }
  {
    long in, fr, pu;
    double el = now() - t0;
    zc_stats(&in, &fr, &pu);
    fprintf(stderr, "zthost: state=%d err='%s' exit=%d  %.1fs  recv %ld bytes (%.0f B/s)  frames %ld  pushed %ld  keys %ld  verify-fail %d\n",
            zc_state(), zc_error(), zc_exit_code(), el, in, in / (el > 0 ? el : 1), fr, pu, kbytes, nbad);
  }
  if (zc_started()) dump(vram, text);
  if (in_open) close(to[1]);
  close(from[0]);
  kill(pid, SIGTERM);
  waitpid(pid, 0, 0);
  return zc_state() == ZC_DONE ? 0 : 1;
}
