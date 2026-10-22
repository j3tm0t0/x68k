/*
 * ztclient.c -- zterm の Z 側: 起動スクリプト・ヘルパの転送・フレームの受け取りと ACK・キー送り。
 * 描画は ztdraw.c (term/vt100 の論理画面)。ssh.x (x68k_shim.c) と Mac の zthost.c が共用する。
 * 動的確保なし。プラットフォームに依るのはヘルパの読み出し (fopen "rb") だけ。
 */
#include <stdio.h>
#include <string.h>
#include "ztclient.h"

#define FRAME_MAX 32768   /* 1 フレームの上限 (サーバの全画面描き直しは 96x32 で十数 KB) */

static int st = ZC_FAIL;
static char z_dir[128], z_ver[40], z_err[240], z_cmd[1400];
static int z_cols, z_rows;
static char line[160];
static int line_len;
static unsigned char fbuf[FRAME_MAX + 3];
static int flen;                 /* fbuf に溜めたバイト数 (ヘッダ 3 バイト込み) */
static int got_hello;
static unsigned char obuf[2048];
static int olen;
static FILE *push_fp;
static long push_left;
static long s_in, s_frames, s_push;
static int z_exit = -1;
static void (*logfn)(const char *);

static void zlog(const char *m) { if (logfn) logfn(m); }
void zc_log(void (*fn)(const char *)) { logfn = fn; }

static void fail(const char *m)
{
  if (st == ZC_FAIL) return;
  strncpy(z_err, m, sizeof z_err - 1); z_err[sizeof z_err - 1] = 0;
  st = ZC_FAIL;
  if (push_fp) { fclose(push_fp); push_fp = 0; }
  zlog(z_err);
}

/* 起動スクリプト。$1=版 $2=桁 $3=行。キャッシュに同じ版が無ければ NEED を出してバイト数の行と本体を待つ。
 * 送る前に --version で動くか確かめる (別アーキテクチャ・壊れた転送を弾く)。
 * ユーザのログインシェルが解釈するので sh -c で包む (fish/tcsh でも通るよう ' ! を本体に使わない)。 */
static const char script[] =
  "V=$1;C=$2;R=$3;D=$HOME/.cache/zterm;A=$(uname -s)-$(uname -m);F=$D/zterm-$V-$A;"
  "if [ -x \"$F\" ]; then echo ZTERM OK $A; else "
  "if command -v gzip >/dev/null 2>&1; then G=gz; else G=raw; fi;"
  "echo ZTERM NEED $A $G;"
  "read N || exit 1;"
  "[ \"$N\" -gt 0 ] 2>/dev/null || { echo ZTERM FAIL nobinary; exit 1; };"
  "mkdir -p \"$D\" || { echo ZTERM FAIL mkdir; exit 1; };"
  "if [ $G = gz ]; then head -c $N | gzip -dc > \"$F.tmp\"; else head -c $N > \"$F.tmp\"; fi;"
  "chmod +x \"$F.tmp\" && \"$F.tmp\" --version >/dev/null 2>&1 && mv -f \"$F.tmp\" \"$F\" || { rm -f \"$F.tmp\"; echo ZTERM FAIL exec; exit 1; };"
  "for O in \"$D\"/zterm-*-$A; do [ \"$O\" = \"$F\" ] || rm -f \"$O\"; done;"
  "fi;"
  "echo ZTERM GO; exec \"$F\" --cols $C --rows $R";

void zc_init(const char *dir, int cols, int rows)
{
  FILE *f;
  char path[160];
  int i;
  st = ZC_HANDSHAKE; line_len = 0; flen = 0; got_hello = 0; olen = 0; push_fp = 0; push_left = 0;
  s_in = s_frames = s_push = 0; z_exit = -1; z_err[0] = 0;
  strncpy(z_dir, dir, sizeof z_dir - 1); z_dir[sizeof z_dir - 1] = 0;
  z_cols = cols; z_rows = rows;
  /* 版は <dir>/VERSION の 1 行目 (ヘルパを作り直したら dist.sh が書き換える) */
  strcpy(z_ver, "0");
  sprintf(path, "%s/VERSION", z_dir);
  f = fopen(path, "rb");
  if (f) {
    if (fgets(z_ver, sizeof z_ver, f)) {
      for (i = 0; z_ver[i]; i++)
        if (!((z_ver[i] >= '0' && z_ver[i] <= '9') || (z_ver[i] >= 'a' && z_ver[i] <= 'z') || z_ver[i] == '.' || z_ver[i] == '-')) { z_ver[i] = 0; break; }
    }
    fclose(f);
  }
  if (!z_ver[0]) strcpy(z_ver, "0");
  sprintf(z_cmd, "exec sh -c '%s' zterm %s %d %d", script, z_ver, cols, rows);
}

const char *zc_command(void) { return z_cmd; }
int zc_state(void) { return st; }
int zc_started(void) { return got_hello; }
const char *zc_error(void) { return z_err; }
int zc_exit_code(void) { return z_exit; }
void zc_stats(long *in_bytes, long *frames, long *push_bytes) { if (in_bytes) *in_bytes = s_in; if (frames) *frames = s_frames; if (push_bytes) *push_bytes = s_push; }

static void oput(const unsigned char *p, int n)
{
  if (olen + n > (int)sizeof obuf) return;   /* あふれ (送れていない): 捨てる */
  memcpy(obuf + olen, p, n); olen += n;
}

/* "ZTERM NEED <arch> gz|raw": <dir>/<arch 小文字>.gz (raw なら拡張子なし) を開いて長さの行を積む */
static void start_push(const char *arch, int gz)
{
  char path[200], name[64], hdr[24];
  int i;
  long sz = 0;
  for (i = 0; arch[i] && i < (int)sizeof name - 1; i++) name[i] = (arch[i] >= 'A' && arch[i] <= 'Z') ? arch[i] + 32 : arch[i];
  name[i] = 0;
  sprintf(path, gz ? "%s/%s.gz" : "%s/%s", z_dir, name);
  push_fp = fopen(path, "rb");   /* 二進 (newlib の既定はテキストモードで 0x0d/0x1a を捨てる) */
  if (push_fp && fseek(push_fp, 0, SEEK_END) == 0) { sz = ftell(push_fp); fseek(push_fp, 0, SEEK_SET); }
  if (!push_fp || sz <= 0) {
    if (push_fp) fclose(push_fp);
    push_fp = 0;
    oput((const unsigned char *)"0\n", 2);   /* スクリプトは FAIL nobinary で終わる */
    sprintf(z_err, "no helper %s", path);
    zlog(z_err);
    st = ZC_WAITGO;
    return;
  }
  sprintf(hdr, "%ld\n", sz);
  oput((const unsigned char *)hdr, (int)strlen(hdr));
  push_left = sz;
  st = ZC_PUSH;
  zlog(path);
}

static void on_line(char *l)
{
  if (strncmp(l, "ZTERM ", 6) != 0) return;   /* .bashrc などの余計な出力は読み飛ばす */
  l += 6;
  if (!strncmp(l, "OK", 2)) { zlog("zterm: cached"); st = ZC_WAITGO; }
  else if (!strncmp(l, "NEED ", 5)) {
    char *a = l + 5, *g = strchr(a, ' ');
    int gz = 1;
    if (g) { *g++ = 0; gz = strncmp(g, "raw", 3) != 0; }
    start_push(a, gz);
  } else if (!strncmp(l, "GO", 2)) { zlog("zterm: go"); st = ZC_RUN; }
  else if (!strncmp(l, "FAIL", 4)) fail(l);
}

static void on_frame(void)
{
  int r;
  if (!got_hello) {
    if (flen < 4 || fbuf[3] != 0x01) { fail("zterm: no hello"); return; }
    got_hello = 1;
    zlog("zterm: hello");
  }
  r = ztd_frame(fbuf + 3, flen - 3);
  s_frames++;
  if (r < 0) { fail("zterm: bad frame"); return; }
  { static const unsigned char ack[2] = { 'a', 0 }; oput(ack, 2); }
  if (r == 1) { z_exit = ztd_bye_code(); st = ZC_DONE; }
}

void zc_input(const unsigned char *p, int n)
{
  s_in += n;
  while (n > 0) {
    if (st == ZC_FAIL || st == ZC_DONE) return;
    if (st != ZC_RUN) {   /* 行の段階 */
      unsigned char b = *p++; n--;
      if (b == '\n') { line[line_len] = 0; line_len = 0; on_line(line); }
      else if (b != '\r' && line_len < (int)sizeof line - 1) line[line_len++] = (char)b;
      continue;
    }
    /* フレーム: 'Z' len16 本体 */
    if (flen < 3) {
      fbuf[flen++] = *p++; n--;
      if (flen == 1 && fbuf[0] != 'Z') { fail("zterm: bad magic"); return; }
      continue;
    }
    {
      int len = (fbuf[1] << 8) | fbuf[2], k = len + 3 - flen;
      if (len > FRAME_MAX) { fail("zterm: frame too big"); return; }
      if (k > n) k = n;
      memcpy(fbuf + flen, p, k); flen += k; p += k; n -= k;
      if (flen == len + 3) { on_frame(); flen = 0; }
    }
  }
}

int zc_pending(void)
{
  return olen + (st == ZC_PUSH && push_fp && olen == 0 ? 1 : 0);
}

int zc_output(unsigned char *p, int max)
{
  int k = 0;
  if (olen > 0) {
    k = olen < max ? olen : max;
    memcpy(p, obuf, k);
    memmove(obuf, obuf + k, olen - k);
    olen -= k;
    return k;
  }
  if (st == ZC_PUSH && push_fp) {
    long want = push_left < max ? push_left : max;
    k = (int)fread(p, 1, (size_t)want, push_fp);
    if (k <= 0) { fail("zterm: read error"); return 0; }
    push_left -= k; s_push += k;
    if (push_left == 0) { fclose(push_fp); push_fp = 0; st = ZC_WAITGO; zlog("zterm: pushed"); }
  }
  return k;
}

void zc_keys(const unsigned char *p, int n)
{
  if (st != ZC_RUN || !got_hello) return;   /* 起動前の打鍵は捨てる (ヘルパの転送に混ぜない) */
  while (n > 0) {
    unsigned char h[2];
    int k = n > 255 ? 255 : n;
    if (olen + 2 + k > (int)sizeof obuf) return;
    h[0] = 'k'; h[1] = (unsigned char)k;
    oput(h, 2); oput(p, k);
    p += k; n -= k;
  }
}

void zc_refresh(void)
{
  static const unsigned char m[2] = { 'f', 0 };
  if (st == ZC_RUN) oput(m, 2);
}
