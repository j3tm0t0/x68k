/*
 * ztclient.c -- zterm の Z 側: 起動スクリプト・ヘルパの転送・フレームの受け取りと ACK・キー送り。
 * 描画は ztdraw.c (term/vt100 の論理画面)。ssh.x (x68k_shim.c) と Mac の zthost.c が共用する。
 * 動的確保なし。プラットフォームに依るのはヘルパの読み出し (fopen "rb") だけ。
 */
#include <stdio.h>
#include <string.h>
#include "ztclient.h"

#define FRAME_MAX 32768   /* 1 フレームの上限 (サーバの全画面描き直しは 96x32 で十数 KB) */

/* 1 本のチャネルの状態。ssh.x の仮想コンソールはチャネルごとに持ち替える (zc_ctx_use)。下の #define で名前はそのまま使う */
typedef struct {
  int st;
  char z_dir[200], z_ver[40], z_err[240], z_cmd[2400];
  int z_cols, z_rows;
  char line[160];
  int line_len;
  unsigned char *fbuf;      /* フレームを溜める (FRAME_MAX + 3 バイト。入れ物の直後、内蔵は fbuf0。32 KB を .data に置かないため) */
  int flen;                 /* fbuf に溜めたバイト数 (ヘッダ 3 バイト込み) */
  int got_hello;
  unsigned char obuf[2048];
  int olen;
  FILE *push_fp;
  long push_left;
  long s_in, s_frames, s_push;
  int z_exit;
  int z_echo;               /* 予測エコー (サーバに ECHOACK を頼む) */
  unsigned long z_kseq;     /* 送ったキーのバイト数 (サーバの ECHOACK と同じ数え方) */
} zc_ctx;
static unsigned char fbuf0[FRAME_MAX + 3];
static zc_ctx zc0 = { .st = ZC_FAIL, .fbuf = fbuf0, .z_exit = -1, .z_echo = 1 }, *zcx = &zc0;
#define st (zcx->st)
#define z_dir (zcx->z_dir)
#define z_ver (zcx->z_ver)
#define z_err (zcx->z_err)
#define z_cmd (zcx->z_cmd)
#define z_cols (zcx->z_cols)
#define z_rows (zcx->z_rows)
#define line (zcx->line)
#define line_len (zcx->line_len)
#define fbuf (zcx->fbuf)
#define flen (zcx->flen)
#define got_hello (zcx->got_hello)
#define obuf (zcx->obuf)
#define olen (zcx->olen)
#define push_fp (zcx->push_fp)
#define push_left (zcx->push_left)
#define s_in (zcx->s_in)
#define s_frames (zcx->s_frames)
#define s_push (zcx->s_push)
#define z_exit (zcx->z_exit)
#define z_echo (zcx->z_echo)
#define z_kseq (zcx->z_kseq)
int zc_ctx_size(void) { return (int)sizeof(zc_ctx) + FRAME_MAX + 3; }
/* 以後の zc_* を ctx (zc_ctx_size バイト) の状態で動かす。NULL なら内蔵の状態。fresh=1 なら ctx を初期状態にする */
void zc_ctx_use(void *ctx, int fresh)
{
  zcx = ctx ? (zc_ctx *)ctx : &zc0;
  if (fresh) { memset(zcx, 0, sizeof *zcx); st = ZC_FAIL; z_exit = -1; z_echo = 1; fbuf = ctx ? (unsigned char *)(zcx + 1) : fbuf0; }
}
void zc_echo(int on) { z_echo = on; ztd_echo(on); }
static void (*logfn)(const char *);

static void zlog(const char *m) { if (logfn) logfn(m); }
static void oput(const unsigned char *p, int n);
void zc_log(void (*fn)(const char *)) { logfn = fn; }

static void fail(const char *m)
{
  if (st == ZC_FAIL) return;
  if (st == ZC_RUN) { static const unsigned char q[2] = { 'q', 0 }; oput(q, 2); }   /* リモートの zterm にも終わってもらう (チャネルが閉じる) */
  if (!z_err[0]) { strncpy(z_err, m, sizeof z_err - 1); z_err[sizeof z_err - 1] = 0; }   /* 最初の原因を残す */
  st = ZC_FAIL;
  if (push_fp) { fclose(push_fp); push_fp = 0; }
  zlog(m);
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
  "echo ZTERM GO; exec \"$F\" --cols $C --rows $R ${4:+-- sh -c \"$4\"}";

void zc_init(const char *dir, int cols, int rows, const char *usercmd)
{
  FILE *f;
  char path[240];
  int i;
  st = ZC_HANDSHAKE; line_len = 0; flen = 0; got_hello = 0; olen = 0; push_fp = 0; push_left = 0;
  s_in = s_frames = s_push = 0; z_exit = -1; z_err[0] = 0; z_kseq = 0;
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
  if (usercmd && *usercmd) {   /* リモートで動かすコマンド ($4)。' は '\'' に */
    char *q = z_cmd + strlen(z_cmd), *end = z_cmd + sizeof z_cmd - 8;
    *q++ = ' '; *q++ = '\'';
    for (; *usercmd && q < end; usercmd++) {
      if (*usercmd == '\'') { *q++ = '\''; *q++ = '\\'; *q++ = '\''; *q++ = '\''; }
      else *q++ = *usercmd;
    }
    *q++ = '\''; *q = 0;
  }
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
  char path[280], name[64], hdr[24];
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
    snprintf(z_err, sizeof z_err, "zterm: no helper %s", path);
    zlog(z_err);
    st = ZC_WAITGO;
    return;
  }
  sprintf(hdr, "%ld\n", sz);
  oput((const unsigned char *)hdr, (int)strlen(hdr));
  push_left = sz;
  st = ZC_PUSH;
  { char m[340]; sprintf(m, "zterm: sending %s (%ld bytes)", path, sz); zlog(m); }
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
  else if (!strncmp(l, "FAIL", 4)) { char m[200]; sprintf(m, "zterm: remote %.150s", l); fail(m); }
}

static void on_frame(void)
{
  int r;
  if (!got_hello) {
    if (flen < 4 || fbuf[3] != 0x01) { fail("zterm: no hello"); return; }
    got_hello = 1;
    zlog("zterm: hello");
    if (z_echo) { static const unsigned char e[2] = { 'e', 0 }; oput(e, 2); }
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
    z_kseq += (unsigned long)k;
    ztd_predict(p, k, z_kseq);
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
