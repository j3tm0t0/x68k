/*
 * x68k_vcons.c -- ssh.x の端末まわり: term/vt100 の画面、zterm (-Z)、仮想コンソール (Ctrl+F1..F4)。
 * x68k_shim.c が #include する (select シムがキーの読み取り kbuf_fill / in_ready を使う)。
 * ホストでの試験 (tests/vcshim.c) は IOCS とチャネルを真似て、このファイルだけを組む。
 */
/* --- VT100 端末 (term/vt100) --- */
#include "vt100.h"
#include <stdarg.h>
#include <errno.h>
#include <sys/ioctl.h>
static int vt_active;                 /* vt_init 済み (vt_init〜vt_fini の間は DOS コンソールに触らない) */
#ifdef X68K_ZTERM
#include "ztclient.h"
static int zt_mode;                   /* -Z: 最初のコンソールは zterm のチャネルで話す */
static int zt_want;                   /* -Z が付いていた */
static int zt_fallback;               /* -Z が始まらず -t に切り替えた (以後のコンソールも -t) */
static int zt_echo = 1;               /* 予測エコー (-oZtermEcho=no で切る) */
static const char *zt_dir = "B:/zterm";
static void zt_keys_fill(void);
static void zt_log(const char *m);
#endif

/* --- 仮想コンソール ---
   1 本の接続にセッションチャネルを最大 VC_N 本開き、それぞれを term/vt100 の端末 (vt_con_*) として持ち、
   Ctrl+F1..F4 / OPT.1+F1..F4 / Ctrl-] 1..4 で表に出すものを切り替える (Linux のコンソールの Alt+F1.. と同じ)。
   ・どのチャネルも fd 0/1/2 を使う (cli_init_stdpipe_sess)。読み書きは x68k_chan_read/write にチャネル番号付きで
     来るので、番号からコンソールを引く (vc_of)。
   ・term/vt100 と zterm の状態は「読み込んでいる」コンソール (vc_ld) のものが大域変数にある。別のコンソールの
     データが来たら持ち替える (vc_use、~1.3 KB の写し)。表 (vc_act) 以外は VRAM に描かないが、解析・zterm の ACK・
     DSR/DA の応答 (そのチャネルへ) は続ける。表に出すとき全面を描き直す (vt_con_visible)。
   ・キーは表のコンソールのチャネルにだけ送る。
   ・まだ無いコンソールに切り替えると、cli_sessionloop (x68k_vc_loop) でセッションチャネルを開く (パケットを作っている
     途中では開かない)。-t なら pty + ログインシェル、-Z なら zterm の起動スクリプト (ヘルパはキャッシュ済み)。
   ・コンソールのシェルが終わったらそのチャネルだけ閉じ、表だったなら別の生きているコンソールに切り替える。
     最後の 1 つが閉じたら今までどおり画面を戻して終わる。 */
#define VC_N 4
#define VC_STATUS_CS 150              /* 切り替えたときの状態表示を出しておく時間 (1/100 秒) */
#define VC_CHAN_MEM (60 * 1024L)      /* 新しいチャネルに要る見込み (dropbear の受信バッファ 2 つ。m_malloc は足りないと終わる) */
typedef struct {
  int used;                           /* コンソールがある */
  int chan;                           /* dropbear のチャネル番号。-1 は x68k_vc_loop が開くのを待っている */
  int ready;                          /* チャネルが開けた (cli_initchansess 済み。それまでキーは溜めるだけ) */
  int zt;                             /* zterm (-Z) で話す */
  void *vst;                          /* 端末の状態 (vt_con_size) */
  void *scr;                          /* 画面 2 面 (vt_con_scrsize)。NULL は vt100.c 内蔵 (最初のコンソール) */
  void *zc, *zd;                      /* zterm の状態。NULL は内蔵 (最初のコンソール) */
  unsigned char kbuf[256];            /* チャネルの stdin へ送るもの (キー、-t の DSR/DA の応答) */
  int kbuf_len;
} vcon_t;
static vcon_t vc[VC_N];
static int vc_act;                    /* 表のコンソール */
static int vc_prev;                   /* その前に表だったコンソール (閉じたときの戻り先) */
static int vc_ld = -1;                /* 大域変数に読み込んでいるコンソール (-1: 無し) */
static int vc_req = -1;               /* キーで頼まれた切り替え先 (vt_key を抜けてから切り替える) */
static int vc_pfx;                    /* 前置キー (Ctrl-]) を押した: 次のキーが 1..4 なら切り替え */
static int vc_esc = 0x1d;             /* 前置キー (-oConsoleEscape=none で 0) */
static long vc_st_t0 = -1;            /* 状態表示を出した時刻 (_iocs_ontime、1/100 秒) */
static int vc_wake;                   /* 開くチャネルがある: select を待たせずに返して cli_sessionloop に回す */
static int vc_zd_owner = -1;          /* ztdraw の状態 (ztd_ctx_use) がいま指しているコンソール (-1: 内蔵) */

static int vc_of(int chan) { int k; for (k = 0; k < VC_N; k++) if (vc[k].used && vc[k].chan == chan) return k; return -1; }
static int vc_live(void) { int k, n = 0; for (k = 0; k < VC_N; k++) n += vc[k].used; return n; }
#ifdef X68K_ZTERM
/* zterm の通信路の状態だけ持ち替える (zc_pending / zc_output / zc_started は端末を使わないので、写しの要る vc_use より軽い) */
static void vc_zc(int k) { if (vc[k].zt) zc_ctx_use(vc[k].zc, 0); }
static void vc_zc_back(void) { if (vc_ld >= 0 && vc[vc_ld].zt) zc_ctx_use(vc[vc_ld].zc, 0); }
#endif
/* コンソール k の端末・zterm の状態を大域変数に読み込む */
static void vc_use(int k)
{
  if (k == vc_ld) return;
  if (vc_ld >= 0) vt_con_save(vc[vc_ld].vst);
  vt_con_load(vc[k].vst);
#ifdef X68K_ZTERM
  if (vc[k].zt) { zc_ctx_use(vc[k].zc, 0); ztd_ctx_use(vc[k].zd, 0); vc_zd_owner = k; }
#endif
  vc_ld = k;
}
#ifdef X68K_ZTERM
/* コンソール k の zterm の入れ物を捨てる前に、ztclient / ztdraw がそれを指さないようにする
   (ztd_ctx_use は持ち替えるとき前の入れ物に書くので、捨てた入れ物を指したままにできない) */
static void vc_zt_forget(int k)
{
  if (vc_zd_owner == k) { ztd_ctx_use(0, 0); vc_zd_owner = -1; }
  zc_ctx_use(0, 0);
  vc_zc_back();
}
#endif
/* 最上行の右端に「1 [2] 3」(表は括弧) を VC_STATUS_CS だけ出す。m があればその後ろに足す */
static void vc_status(const char *m)
{
  char b[64], *q = b;
  int k;
  if (!vt_active || !vc[vc_act].used) return;
  *q++ = ' ';
  for (k = 0; k < VC_N; k++) {
    if (!vc[k].used) continue;
    if (k == vc_act) { *q++ = '['; *q++ = (char)('1' + k); *q++ = ']'; }
    else { *q++ = (char)('1' + k); }
    *q++ = ' ';
  }
  if (m) { strncpy(q, m, b + sizeof b - 2 - q); b[sizeof b - 2] = 0; q += strlen(q); *q++ = ' '; }
  *q = 0;
  vc_use(vc_act);
  vt_con_status(b);
  vc_st_t0 = _iocs_ontime().sec;
}
static void vc_status_tick(void)
{
  long t;
  if (vc_st_t0 < 0) return;
  t = _iocs_ontime().sec;
  if (t - vc_st_t0 < VC_STATUS_CS && t >= vc_st_t0) return;
  vc_st_t0 = -1;
  if (vt_active && vc[vc_act].used) { vc_use(vc_act); vt_con_status(0); }
}
static void vc_free(vcon_t *v)
{
  free(v->vst); free(v->scr); free(v->zc); free(v->zd);
  v->vst = v->scr = v->zc = v->zd = 0;
}
/* 新しいコンソール n を作る (裏に置く)。チャネルは x68k_vc_loop が開く。メモリが足りなければ 0 */
static int vc_open(int n)
{
  vcon_t *v = &vc[n];
  void *probe;
  memset(v, 0, sizeof *v);
  v->vst = malloc(vt_con_size());
  v->scr = malloc(vt_con_scrsize());
#ifdef X68K_ZTERM
  v->zt = zt_want && !zt_fallback;
  if (v->zt) { v->zc = malloc(zc_ctx_size()); v->zd = malloc(ztd_ctx_size()); }
  if (v->zt && (!v->zc || !v->zd)) { vc_free(v); return 0; }
#endif
  if (!v->vst || !v->scr) { vc_free(v); return 0; }
  probe = malloc(VC_CHAN_MEM);   /* この後 dropbear がチャネルの受信バッファを取る分も残っているか */
  if (!probe) { vc_free(v); return 0; }
  free(probe);
  if (vc_ld >= 0) vt_con_save(vc[vc_ld].vst);
  vt_con_new(v->scr);
  vc_ld = n;
#ifdef X68K_ZTERM
  if (v->zt) {
    int c, r;
    zc_ctx_use(v->zc, 1); ztd_ctx_use(v->zd, 1); vc_zd_owner = n;
    vt_get_size(&c, &r);
    zc_init(zt_dir, c, r, 0);   /* ログインシェル */
    zc_echo(zt_echo);
    zc_log(zt_log);
  }
#endif
  v->used = 1;
  v->chan = -1;
  vc_wake = 1;
  return 1;
}
/* 表を n にする (無ければ作る) */
static void vc_switch(int n)
{
  if (n < 0 || n >= VC_N || !vt_active) return;
  if (!vc[n].used && !vc_open(n)) { vc_status("no memory"); return; }
  if (n != vc_act) {
    vc_use(vc_act); vt_con_visible(0);
    vc_prev = vc_act; vc_act = n;
    vc_use(n); vt_con_visible(1);
  }
  vc_status(0);
}
/* コンソール k を捨てる (チャネルは閉じた)。表だったなら別のコンソールを表に */
static void vc_drop(int k)
{
  vcon_t *v = &vc[k];
  if (vc_ld == k) vc_ld = -1;
#ifdef X68K_ZTERM
  if (v->zt) { v->zt = 0; vc_zt_forget(k); }
#endif
  vc_free(v);
  v->used = 0; v->chan = -1;
  if (vc_act == k) {
    int n = vc[vc_prev].used ? vc_prev : -1, j;
    for (j = 0; n < 0 && j < VC_N; j++) if (vc[j].used) n = j;
    if (n < 0) return;
    vc_act = vc_prev = n;
    vc_use(n); vt_con_visible(1);
  }
  vc_status(0);
}
/* vt_key のキーフック: 切り替えのキーを取る (チャネルには送らない) */
static int vc_keyhook(int code)
{
  int scan = (code >> 8) & 0xff, a = code & 0xff;
  if (scan >= 0x63 && scan < 0x63 + VC_N && (_iocs_b_sftsns() & 0x06)) { vc_req = scan - 0x63; return 2; }   /* CTRL / OPT.1 + F1..F4 */
  if (vc_esc && a == vc_esc) { vc_pfx = 1; return 2; }
  return 0;
}
/* 表のチャネルへ 1 バイト送る (前置キーそのもの) */
static void vc_send_byte(unsigned char b)
{
  vcon_t *v = &vc[vc_act];
#ifdef X68K_ZTERM
  if (v->zt) { zc_keys(&b, 1); return; }
#endif
  if (v->kbuf_len < (int)sizeof v->kbuf) v->kbuf[v->kbuf_len++] = b;
}
/* 前置キーの次のキー: 1..4 なら切り替え、もう一度前置キーならそれを 1 つ送る、他は前置キーを送ってそのキーは普通に */
static void vc_prefix(void)
{
  int code = _iocs_b_keysns(), a;
  if (!code) return;
  a = code & 0xff;
  vc_pfx = 0;
  if (a >= '1' && a < '1' + VC_N) { _iocs_b_keyinp(); vc_req = a - '1'; return; }
  if (a == vc_esc) _iocs_b_keyinp();
  vc_send_byte((unsigned char)vc_esc);
}

/* キーボードを非ブロッキングで読み、表のコンソールの送信待ちに足す。切り替えのキーもここで処理する */
static void kbuf_fill(void)
{
  vcon_t *v;
  if (!vc[vc_act].used) return;
  vc_use(vc_act);
  v = &vc[vc_act];
  if (vc_pfx) vc_prefix();
  if (vc_req < 0) {
#ifdef X68K_ZTERM
    if (v->zt) zt_keys_fill(); else
#endif
    if (v->kbuf_len < (int)sizeof v->kbuf)
      v->kbuf_len += vt_key(v->kbuf + v->kbuf_len, (int)sizeof v->kbuf - v->kbuf_len);
  }
  if (vc_req >= 0) { int n = vc_req; vc_req = -1; vc_switch(n); }
  vc_status_tick();
}
/* どれかのコンソールにチャネルの stdin へ送るものがあるか (キー / 応答 / zterm の ACK・転送)。fd 0 は全チャネル共通 */
static int in_ready(void)
{
  int k, r = 0;
  for (k = 0; k < VC_N && !r; k++) {
    if (!vc[k].used || !vc[k].ready) continue;
#ifdef X68K_ZTERM
    if (vc[k].zt) { vc_zc(k); r = zc_pending() > 0; continue; }
#endif
    r = vc[k].kbuf_len > 0;
  }
#ifdef X68K_ZTERM
  vc_zc_back();
#endif
  return r;
}

/* cli_initchansess (チャネルが開けた) から: そのチャネルのコンソールの pty 要求とコマンドを決める。
   最初のチャネル (cli_send_chansess_request が開いたもの) はコンソール 1 にして cli_opts のまま */
void x68k_vc_initchan(int chan, int *wantpty, char **cmd)
{
  int k = vc_of(chan);
  if (k >= 0) vc[k].ready = 1;
  if (k < 0) {
    if (vc_live() > 0) return;
    memset(&vc[0], 0, sizeof vc[0]);
    vc[0].used = 1; vc[0].chan = chan; vc[0].ready = 1;
#ifdef X68K_ZTERM
    vc[0].zt = zt_mode;
#endif
    vc[0].vst = malloc(vt_con_size());
    if (!vc[0].vst) { extern void dropbear_exit(const char *, ...); dropbear_exit("no memory"); }
    vc_act = vc_prev = vc_ld = 0;
    vt_keyhook = vc_keyhook;
    return;
  }
#ifdef X68K_ZTERM
  if (vc[k].zt) {
    vc_zc(k);
    *wantpty = 0; *cmd = (char *)zc_command();   /* 入れ物の中の文字列 (コンソールが閉じるまで有効) */
    vc_zc_back();
    return;
  }
#endif
  *wantpty = 1; *cmd = 0;                      /* ログインシェル */
}

/* cli_sessionloop (SESSION_RUNNING) から: 開く約束のコンソールのチャネルを開く */
void x68k_vc_loop(void)
{
  extern int x68k_open_session(void);
  int k;
  vc_wake = 0;
  for (k = 0; k < VC_N; k++)
    if (vc[k].used && vc[k].chan == -1) {
      int c = x68k_open_session();
      if (c < 0) { vc_drop(k); vc_status("open failed"); }
      else vc[k].chan = c;
    }
}

/* --- ioctl: TIOCGWINSZ だけ VT の寸法 (96x32) を返す。他は -1 --- */
int ioctl(int fd, unsigned long request, ...)
{
  (void)fd;
  if (request == TIOCGWINSZ) {
    va_list ap; struct winsize *ws; int c, r;
    va_start(ap, request); ws = va_arg(ap, struct winsize *); va_end(ap);
    vt_get_size(&c, &r);           /* vt_init 前でも既定の 96x32 が返る */
    if (ws) { ws->ws_col = (unsigned short)c; ws->ws_row = (unsigned short)r; ws->ws_xpixel = (unsigned short)(c * 8); ws->ws_ypixel = (unsigned short)(r * 16); }
    return 0;
  }
  return -1;
}

/* cli_tty_setup (pty 要求後、シェル要求の後) から: 画面を VT100 に切り替える (2 つ目以降のコンソールでは何もしない) */
void x68k_tty_setup(void)
{
  if (vt_active) return;
  vt_init(0, 0);
  vt_active = 1;
}

/* cli_tty_cleanup (チャネル close / 終了処理) から: コンソールを戻す。画面は消さず最後の表示を残す。
   Human68k のコンソールはプレーン 0,1 しか使わず、スクロール (IOCS) もプレーン 0,1 しか動かさない
   (実機で確認: 2,3 は置き去りになり、色付きの残骸が 1 行ずれて重なる)。そこで残った画面を
   「点灯ピクセルは全部プレーン 0,1 (コンソールの白)、2,3 は空」に潰してから戻す。
   IOCS のカーソルは VT のカーソル行の次 (行末なら次行) に置くが、コンソールの最終行 (CON_ROWS-1、
   ファンクションキー行があるので 31 行) を越えるなら自分で全プレーンをスクロールして空ける。
   IOCS に改行させると上記の片寄りスクロールになるので、ここでは改行を出さない。
   VTKEEP=1 ./build.sh (-DX68K_VT_KEEP) の版は潰しもスクロールもしない (VRAM ダンプで色を確かめる試験用。
   rcmd の環境ブロックは満杯で set が効かないので環境変数でなくビルド時に切り替える)。 */
#define CON_ROWS 31
#ifndef TVRAM_BASE   /* ホスト試験は term/vt100 のホスト版 VRAM */
#define TVRAM_BASE ((volatile unsigned char *)0xE00000UL)
#endif
#define TVRAM_PLANE 0x20000UL
void x68k_tty_cleanup(void)
{
  int x, y, cols, rows, row, s, line;
  if (!vt_active) return;
  if (vc[vc_act].used) vc_use(vc_act);   /* 残すのは表のコンソールの画面 */
  vt_get_cursor(&x, &y);
  vt_get_size(&cols, &rows);
  vt_fini();
  vt_active = 0;
  row = (x > 0) ? y + 1 : y;
#ifdef X68K_VT_KEEP
  _iocs_b_locate(0, row < CON_ROWS ? row : CON_ROWS - 1); return;
#endif
  s = _dos_super(0);
  for (line = 0; line < rows * 16; line++) {
    volatile unsigned char *p0 = TVRAM_BASE + (unsigned long)line * 128;
    volatile unsigned char *p1 = p0 + TVRAM_PLANE, *p2 = p0 + 2 * TVRAM_PLANE, *p3 = p0 + 3 * TVRAM_PLANE;
    int c;
    for (c = 0; c < cols; c++) {
      unsigned char b = (unsigned char)(p0[c] | p1[c] | p2[c] | p3[c]);
      p0[c] = b; p1[c] = b; p2[c] = 0; p3[c] = 0;
    }
  }
  if (row > CON_ROWS - 1) {
    int n = row - (CON_ROWS - 1);        /* 空けたい行数 */
    int r, c;
    if (n < rows) _iocs_txrascpy(((n * 4) << 8) | 0, (rows - n) * 4, 0x000f);   /* 上へ n 行 */
    for (r = rows - n; r < rows; r++)
      for (line = r * 16; line < r * 16 + 16; line++)
        for (c = 0; c < cols; c++) { TVRAM_BASE[(unsigned long)line * 128 + c] = 0; TVRAM_BASE[TVRAM_PLANE + (unsigned long)line * 128 + c] = 0; }
    row = CON_ROWS - 1;
  }
  if (s >= 0) _dos_super(s);
  _iocs_b_locate(0, row);
}

/* チャネル chan の fd 書き込み (writechannel から)。stdout/stderr はそのコンソールの端末へ (裏なら描かずに解析だけ) */
int x68k_chan_write(int chan, int fd, const void *p, unsigned int n)
{
  int k;
  if (fd >= X68K_SOCKFD_MIN) return (int)write(fd, p, n);
  k = vc_of(chan);
  if (k < 0) return vt_active ? (int)n : (int)write(fd, p, n);   /* 閉じたコンソールの残り: 捨てる */
  vc_use(k);
#ifdef X68K_ZTERM
  if (vc[k].zt) {
    extern int x68k_zterm_write(int, const void *, unsigned int);
    return x68k_zterm_write(fd, p, n);
  }
#endif
  if (vt_active) {
    vcon_t *v = &vc[k];
    vt_write((const unsigned char *)p, (int)n);
    v->kbuf_len += vt_reply(v->kbuf + v->kbuf_len, (int)sizeof v->kbuf - v->kbuf_len);   /* DSR/DA の応答はこのチャネルへ */
    return (int)n;
  }
  return (int)write(fd, p, n);
}

/* チャネル chan の fd 読み出し (send_msg_channel_data から)。stdin はそのコンソールの送信待ちから。
   0 や EAGAIN を返すと dropbear は EOF 扱いで stdin を閉じるので、空なら -1/EINTR。 */
int x68k_chan_read(int chan, int fd, void *p, unsigned int n)
{
  int k;
  vcon_t *v;
  if (fd >= X68K_SOCKFD_MIN) return (int)read(fd, p, n);
  k = vc_of(chan);
  if (k < 0) { errno = EINTR; return -1; }
  if (k == vc_act) kbuf_fill();
  v = &vc[k];
#ifdef X68K_ZTERM
  if (v->zt) {
    int r;
    vc_zc(k);
    r = zc_output((unsigned char *)p, (int)n);
    vc_zc_back();
    if (r <= 0) { errno = EINTR; return -1; }
    return r;
  }
#endif
  if (v->kbuf_len == 0) { errno = EINTR; return -1; }
  {
    unsigned int m = (unsigned int)v->kbuf_len < n ? (unsigned int)v->kbuf_len : n;
    memcpy(p, v->kbuf, m);
    memmove(v->kbuf, v->kbuf + m, v->kbuf_len - m);
    v->kbuf_len -= m;
    return (int)m;
  }
}

/* チャネルが閉じた (cli_closechansess から)。0 を返したら最後のコンソール: 呼び出し側が画面を戻す。
   1: 他のコンソールが残っている (表なら切り替えた) / zterm が始まらなかったので同じコンソールを -t で開き直す */
int x68k_vc_closed(int chan)
{
  int k = vc_of(chan);
  if (k < 0) return 0;
#ifdef X68K_ZTERM
  if (vc[k].zt) {
    int started;
    const char *err;
    vc_zc(k);
    started = zc_started();
    err = zc_error()[0] ? zc_error() : "helper did not start";
    if (!started) {
      if (!vt_active) {
        _iocs_b_print("zterm: "); _iocs_b_print(err); _iocs_b_print(" -- falling back to -t\r\n");
      }
      vc[k].zt = 0; zt_fallback = 1;
      vc_zt_forget(k);
      free(vc[k].zc); free(vc[k].zd); vc[k].zc = vc[k].zd = 0;
      vc[k].chan = -1; vc[k].ready = 0;   /* x68k_vc_loop が -t で開き直す */
      vc_wake = 1;
      if (vt_active) vc_status("zterm failed, -t");
      return 1;
    }
    vc_zc_back();
  }
#endif
  if (vc_live() > 1) { vc_drop(k); return 1; }
  /* 最後のコンソール */
  vc_use(k);
#ifdef X68K_ZTERM
  if (vc[k].zt) {
    long h, m;
    char b[64];
    x68k_tty_cleanup();
    ztd_echo_stats(&h, &m);
    if (h || m) { sprintf(b, "zterm: local echo %ld hit / %ld miss\r\n", h, m); _iocs_b_print(b); }
  }
#endif
  return 0;
}

#ifdef X68K_ZTERM
/* --- zterm (term/zterm): リモートのヘルパが画面を持ち、変わったセルだけ送ってくる ---
   ssh.x -Z host [cmd]: pty を要求せず、1 本の exec チャネルで起動スクリプトを動かす (ztclient.c)。
   チャネルの stdout は zc_input (行の段階 -> フレーム)、stdin は zc_output (ヘルパの転送・ACK・キー)。
   画面は最初のフレーム (HELLO) で ztdraw.c が vt_init する。ヘルパは ZTERMDIR (既定 B:/zterm) の
   <uname -s>-<uname -m>.gz (小文字) と VERSION。始まらずにチャネルが閉じたら、そのコンソールを -t の対話セッションで
   開き直す (x68k_vc_closed)。仮想コンソールはコンソールごとに zterm の状態を持つ (zc_ctx_use / ztd_ctx_use)。 */
/* 位相の印 (cached / helper のパス / pushed / go / エラー)。画面を占有する前だけ IOCS で直接出す
   (チャネルが閉じる頃には dropbear が fd 2 を閉じていることがあるので stderr は使わない) */
static void zt_log(const char *m)
{
  x68dbg(m);
#ifndef X68K_SSH_DEBUG
  if (!vt_active) { _iocs_b_print(m); _iocs_b_print("\r\n"); }
#endif
}
#ifdef X68K_ZT_SNAP
/* 試験用 (ZTSNAP=1): 予測を 3 つ以上描いた最初のときに、テキスト VRAM (4 プレーン x 512 ライン) とパレットを
   B:/ztsnap.bin に書く (term/vt100/tests/vram2png.py の形)。打鍵の途中の Z の画面を見るため (rcmd の memdump は
   ssh.x の実行中は使えない)。書く間 (数秒) は止まる。 */
static void zt_snap(void)
{
  static int done;
  static unsigned char line[128];
  FILE *f;
  int p, y, i, s;
  if (done || ztd_echo_shown() < 3) return;
#ifdef X68K_ZT_SNAP_ROW
  { int x, y, v; ztd_cursor(&x, &y, &v); if (y < X68K_ZT_SNAP_ROW) return; }   /* その行より下 (入力欄) で打っているときだけ */
#endif
  done = 1;
  f = fopen("B:/ztsnap.bin", "wb");
  if (!f) return;
  for (p = 0; p < 4; p++)
    for (y = 0; y < 512; y++) {
      s = _dos_super(0);
      for (i = 0; i < 128; i++) line[i] = TVRAM_BASE[(unsigned long)p * TVRAM_PLANE + (unsigned long)y * 128 + i];
      if (s >= 0) _dos_super(s);
      fwrite(line, 1, 128, f);
    }
  s = _dos_super(0);
  for (i = 0; i < 16; i++) { unsigned short w = ((volatile unsigned short *)0xE82200UL)[i]; line[i * 2] = (unsigned char)(w >> 8); line[i * 2 + 1] = (unsigned char)w; }
  if (s >= 0) _dos_super(s);
  fwrite(line, 1, 32, f);
  fclose(f);
}
#endif
static void zt_keys_fill(void)
{
  unsigned char t[64];
  int k = vt_key(t, (int)sizeof t);
  if (k > 0) zc_keys(t, k);
#ifdef X68K_ZT_SNAP
  if (k > 0) zt_snap();
#endif
}

/* cli-main.c (getopts の前) から: -Z を argv から抜く。ホスト名 (値を取らない最初の非オプション) より後は
   リモートのコマンドなので触らない (ssh.x -Z host grep -Z ... の -Z は残す) */
int x68k_zterm_args(int *argc, char **argv)
{
  static const char valopt[] = "pilLRocmWKIJbB";   /* 値を取る dbclient のオプション */
  int i, j, host = 0;
  for (i = j = 0; i < *argc; i++) {
    const char *a = argv[i];
    if (i > 0 && !host) {
      if (!strcmp(a, "-Z")) { zt_want = 1; continue; }
      if (!strcmp(a, "-oZtermEcho=no")) { zt_echo = 0; continue; }    /* 予測エコーを使わない */
      if (!strcmp(a, "-oZtermEcho=yes")) { zt_echo = 1; continue; }
      if (!strncmp(a, "-oConsoleEscape=", 16)) {   /* 仮想コンソールの前置キー: none / ^X (既定 ^]) */
        const char *e = a + 16;
        vc_esc = (e[0] == '^' && e[1]) ? (e[1] & 0x1f) : 0;
        continue;
      }
      if (a[0] == '-' && a[1] && !a[2] && strchr(valopt, a[1]) && i + 1 < *argc) { argv[j++] = argv[i++]; a = argv[i]; }
      else if (a[0] != '-') host = 1;
    }
    argv[j++] = (char *)a;
  }
  argv[j] = 0;
  *argc = j;
  return zt_want;
}

/* cli-main.c (getopts の後) から: exec するコマンド (起動スクリプト) を返す。-Z が無ければ NULL */
const char *x68k_zterm_command(const char *usercmd)
{
  const char *dir = getenv("ZTERMDIR");
  int c, r;
  if (!zt_want) return 0;
  if (dir) zt_dir = dir;
  vt_default_size(&c, &r);
  zc_init(zt_dir, c, r, usercmd);
  zc_echo(zt_echo);
  zc_log(zt_log);
  zt_mode = 1;
  return zc_command();
}

/* 読み込んでいるコンソール (zterm) のチャネルの stdout / stderr */
int x68k_zterm_write(int fd, const void *p, unsigned int n)
{
  if (fd == 1) {
    zc_input((const unsigned char *)p, (int)n);
    if (zc_started() && !vt_active) vt_active = 1;   /* HELLO で画面を占有した (以後 DOS コンソールに触らない) */
    if (zc_state() == ZC_FAIL && vt_active && vc_live() == 1) {
      /* 動き出した後に通信路が壊れた: 画面を戻して終わる (他にコンソールがあれば、fail が送った 'q' でこのチャネルだけ閉じる) */
      extern void dropbear_exit(const char *, ...);
      x68k_tty_cleanup();
      dropbear_exit("zterm: %s", zc_error());
    }
    return (int)n;
  }
  /* stderr: 始まる前 (起動スクリプトのエラー) はコンソールへ、描画中は捨てる */
  if (!vt_active) return (int)write(fd, p, n);
  return (int)n;
}
#endif

