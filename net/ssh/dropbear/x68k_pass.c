/*
 * x68k_pass.c -- パスワード認証・keyboard-interactive の入力 (dropbear の getpass / get_response の代わり)。
 * x68k_shim.c が #include する (tests/passtest.c も DOS を真似て #include する)。
 *
 * 認証は端末の層 (vt_init: -t / -Z、仮想コンソール) が画面を取る前に終わるので、プロンプトは素の Human68k コンソール
 * (fd 2、CTTY に従う) に出し、キーも DOS の標準入力から _INPOUT(0xFF) (待たない・エコー無し・ブレークチェック無し) で
 * 1 つずつ取る。IOCS の _B_KEYINP で直接読むと、プロンプトや "Permission denied" を DOS で書く間に打ったキーを DOS が
 * 自分の先読みに移してしまい、そのキーが抜ける (XEiJ で "bad3" が "ad3" になり、'b' は終了後の A> に出た)。
 * 空の間は _dos_change_pr で xip に譲る。
 *   Enter: 確定  BS / DEL: 1 字消す  Ctrl-U: 全部消す  ESC / Ctrl-C: 取り消し (NULL を返す。呼び出し側が "Interrupted." で終わる)
 *   それ以外の制御文字 (カーソルキーは KEY.SYS で ^E ^X ^S ^D になる) は捨てる。echo=0 なら何も表示しない。
 * 入力中は _dos_breakck(0): Ctrl-C で DOS がプロセスを止めると、xip のソケットが開いたまま残るため。
 * 返すのは静的バッファ。dropbear は使い終わると m_burn する。取り消し・次の呼び出しでもここで消す。
 */
#define X68K_PASS_MAX 256
static char x68k_pass_buf[X68K_PASS_MAX];

static void x68k_pass_burn(void)
{
  volatile char *p = x68k_pass_buf;
  int i;
  for (i = 0; i < X68K_PASS_MAX; i++) p[i] = 0;
}

static void x68k_pass_out(const char *s, int n) { if (n > 0) (void)write(2, s, n); }

char *x68k_readpass(const char *prompt, int echo)
{
  int n = 0, brk, c;
  x68k_pass_burn();
  brk = _dos_breakck(-1);
  _dos_breakck(0);
  x68k_pass_out(prompt, (int)strlen(prompt));
  for (;;) {
    c = _dos_inpout(0xff) & 0xff;
    if (c == 0) { _dos_change_pr(); continue; }
    if (c == '\r' || c == '\n') break;
    if (c == 0x1b || c == 0x03) {
      x68k_pass_burn();
      x68k_pass_out("\r\n", 2);
      _dos_breakck(brk);
      return NULL;
    }
    if (c == 0x08 || c == 0x7f) {
      if (n > 0) { x68k_pass_buf[--n] = 0; if (echo) x68k_pass_out("\b \b", 3); }
      continue;
    }
    if (c == 0x15) {
      while (n > 0) { x68k_pass_buf[--n] = 0; if (echo) x68k_pass_out("\b \b", 3); }
      continue;
    }
    if (c < 0x20) continue;
    if (n < X68K_PASS_MAX - 1) {
      x68k_pass_buf[n++] = (char)c;
      if (echo) x68k_pass_out(&x68k_pass_buf[n - 1], 1);
    }
  }
  x68k_pass_out("\r\n", 2);
  _dos_breakck(brk);
  return x68k_pass_buf;
}
