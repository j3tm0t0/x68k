/*
 * rx.x -- RS-232C だけで X68000 にファイルを送り込み、コマンドを実行するサーバ (boot/serial)
 *
 *   rx            サーバとして動く。送る側 (sx.py) からの要求に 1 つずつ答える
 *   rx -b19200    RS-232C を 19200bps 8N1 フロー制御なしにして終わる (AUTOEXEC.BAT の CTTY AUX の前に使う)
 *   rx -b19200 -s 速度を変えてからサーバとして動く
 *   rx -v         画面に要求を 1 行ずつ表示する
 *   rx -m         いま確保できる最大のメモリの大きさを表示する
 *   rx -k2        2 秒待ち、その間に X68000 のキーが押されたら終了コード 1 で終わる (押されなければ 0)。
 *                 AUTOEXEC.BAT で IF ERRORLEVEL 1 と組み合わせ、CTTY AUX を飛ばしてキーボードを残すのに使う
 *
 * 画面への表示は IOCS で直接行う (CTTY AUX のときに標準出力へ書くと通信に混ざるため)。
 * X68000 のキーボードで ESC を押すとサーバを止める。
 *
 * 通信: 要求も応答も同じ枠 SOH(01) 種類 番号 長さ(2, 上位から) 中身 CRC16(2)
 *   CRC は CCITT (多項式 1021, 初期値 FFFF) を種類から中身の終わりまでで計算する。
 *   送る側が要求を送り、rx は必ず 1 つ応答する ('K' = 成功、'E' = 失敗、'N' = 枠が壊れていた)。
 *   同じ番号の要求がもう一度来たら、実行し直さず前の応答をそのまま返す (応答が壊れたときの再送用)。
 * 要求の種類:
 *   H            あいさつ。応答は "RX <版> <最大長>"
 *   O 方式 名前  書くために開く (方式 0 = 作り直す、1 = 末尾に足す)
 *   W データ     開いたファイルに書く
 *   C            閉じる
 *   P 名前       読むために開く。応答は大きさ (4)
 *   R 長さ(2)    読む。応答は読んだデータ (0 バイトなら終わり)
 *   X 行         COMMAND.X /C 行 を実行し、標準出力と標準エラー出力をファイルに落とす (標準入力は NUL)。
 *                応答は 終了コード(4) とファイル名
 *   B モード(2)  応答を返してから IOCS _SET232C でモードを変える
 *   S            統計 (受信バッファに溜まった最大バイト数 (4)、壊れていた枠の数 (4))。最大値は 0 に戻す
 *   Q            応答を返して終わる
 */
#include <x68k/dos.h>
#include <x68k/iocs.h>

#define VERSION "1"
#define MAXLEN 4096

static unsigned short crctab[256];
static unsigned char rbuf[MAXLEN + 8];          /* 受けた要求 */
static unsigned char sbuf[MAXLEN + 8];          /* 最後に送った応答 (再送用) */
static int slen;
static int last_seq = -1, last_type = -1;
static int wfd = -1, rfd = -1;
static int verbose;
static int maxlof;                      /* 受信バッファに溜まっていた最大のバイト数 (S で返す) */
static long nbad;                       /* 壊れていた枠の数 */
static char outname[] = "A:\\RX.OUT";
static char execbuf[300];
static struct dos_comline comline;

static void put(const char *s) { _iocs_b_print(s); }

/* いま確保できる最大のメモリブロックの大きさ (_MALLOC に大きすぎる値を渡すと、0x81000000 | 最大の大きさ が返る) */
static long freemem(void)
{
  long r = (long)_dos_malloc(0x00ffffff);
  if (r >= 0) { _dos_mfree((void *)r); return 0x00ffffff; }
  return r & 0x00ffffff;
}

static void putnum(long v)
{
  char b[12], *p = b + 11;
  int neg = v < 0;
  unsigned long u = neg ? -v : v;
  *p = 0;
  do { *--p = '0' + u % 10; u /= 10; } while (u);
  if (neg) *--p = '-';
  put(p);
}

static void crcinit(void)
{
  int i, j;
  for (i = 0; i < 256; i++) {
    unsigned short c = i << 8;
    for (j = 0; j < 8; j++)
      c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    crctab[i] = c;
  }
}

static unsigned short crc(const unsigned char *p, int n)
{
  unsigned short c = 0xffff;
  while (n--)
    c = (c << 8) ^ crctab[(c >> 8) ^ *p++];
  return c;
}

/* 1/100 秒の時計 (日付が変わると 0 に戻るので差は 1 日で折り返す) */
static long now(void) { return _iocs_ontime().sec; }
static long since(long t)
{
  long d = now() - t;
  if (d < 0) d += 8640000;
  return d;
}

static int esc_pressed(void)
{
  if (_iocs_b_keysns()) {
    int k = _iocs_b_keyinp();
    if ((k & 0xff) == 0x1b)
      return 1;
  }
  return 0;
}

/* 1 バイト読む。timeout (1/100 秒) 内に来なければ -1。ESC が押されたら -2 */
static int getb(long timeout)
{
  long t = now();
  int n = 0;
  for (;;) {
    int l = _iocs_lof232c();
    if (l > 0) {
      if (l > maxlof) maxlof = l;
      return _iocs_inp232c() & 0xff;
    }
    if (timeout >= 0 && since(t) > timeout)
      return -1;
    if (++n >= 2000) {
      n = 0;
      if (esc_pressed())
        return -2;
    }
  }
}

static void send(int type, int seq, const void *data, int len)
{
  unsigned short c;
  int i;
  sbuf[0] = 1;
  sbuf[1] = type;
  sbuf[2] = seq;
  sbuf[3] = len >> 8;
  sbuf[4] = len;
  if (data && len)
    __builtin_memcpy(sbuf + 5, data, len);
  c = crc(sbuf + 1, len + 4);
  sbuf[5 + len] = c >> 8;
  sbuf[6 + len] = c;
  slen = len + 7;
  for (i = 0; i < slen; i++)
    _iocs_out232c(sbuf[i]);
}

/* 枠が壊れていたことを知らせる。最後の応答 (sbuf) は再送用に残す */
static void nak(int seq)
{
  static unsigned char b[7] = { 1, 'N', 0, 0, 0, 0, 0 };
  unsigned short c;
  int i;
  b[2] = seq;
  c = crc(b + 1, 4);
  b[5] = c >> 8;
  b[6] = c;
  for (i = 0; i < 7; i++)
    _iocs_out232c(b[i]);
}

static void resend(void)
{
  int i;
  for (i = 0; i < slen; i++)
    _iocs_out232c(sbuf[i]);
}

static void put32(unsigned char *p, long v)
{
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* COMMAND.X /C line を実行し、出力を outname に落とす。戻り値は終了コード (負なら起動の失敗) */
static long run(const char *line, int len)
{
  int fd, si, so, se, nul, i;
  long rc;
  const char *pre = "command.x /c ";
  char *p = execbuf;

  if (len > 240) len = 240;
  for (i = 0; pre[i]; i++) *p++ = pre[i];
  for (i = 0; i < len; i++) *p++ = line[i];
  *p = 0;
  fd = _dos_create(outname, 0x20);
  if (fd < 0)
    return fd;
  si = _dos_dup(0);
  so = _dos_dup(1);
  se = _dos_dup(2);
  _dos_dup2(fd, 1);
  _dos_dup2(fd, 2);
  _dos_close(fd);
  /* 標準入力は NUL にする (CTTY AUX のとき、子が標準入力を読むと RS-232C の通信を横取りする) */
  if ((nul = _dos_open("NUL", 0)) >= 0) {
    _dos_dup2(nul, 0);
    _dos_close(nul);
  }
  rc = _dos_exec2(2, execbuf, (const char *)&comline, 0);   /* PATH を探して execbuf を実行ファイル名と引数に分ける */
  if (rc >= 0)
    rc = _dos_loadexec(execbuf, &comline, 0);
  _dos_dup2(si, 0);
  _dos_dup2(so, 1);
  _dos_dup2(se, 2);
  _dos_close(si);
  _dos_close(so);
  _dos_close(se);
  return rc;
}

static void cstr(char *dst, const unsigned char *src, int len, int max)
{
  if (len > max - 1) len = max - 1;
  __builtin_memcpy(dst, src, len);
  dst[len] = 0;
}

static int serve(void)
{
  static char name[96];
  unsigned char tmp[8];
  int c, i, type, seq, len, bad = 0;
  long n;

  put("rx: ready (ESC to stop)\r\n");
  send('K', 0, "RX " VERSION, 4);   /* 起動の合図 (送る側は H で確かめ直す) */
  for (;;) {
    /* SOH を待つ */
    do {
      c = getb(-1);
      if (c == -2) { put("rx: ESC\r\n"); return 1; }
    } while (c != 1);
    for (i = 0; i < 4; i++) {
      if ((c = getb(100)) < 0) break;
      rbuf[i] = c;
    }
    if (c < 0) continue;
    type = rbuf[0];
    seq = rbuf[1];
    len = (rbuf[2] << 8) | rbuf[3];
    if (len > MAXLEN) { nak(seq); bad++; continue; }
    for (i = 0; i < len + 2; i++) {
      if ((c = getb(100)) < 0) break;
      rbuf[4 + i] = c;
    }
    if (c < 0 || crc(rbuf, len + 4) != ((rbuf[4 + len] << 8) | rbuf[5 + len])) {
      bad++;
      nbad++;
      if (verbose) { put("rx: bad frame "); putnum(bad); put("\r\n"); }
      nak(seq);
      continue;
    }
    if (seq == last_seq && type == last_type) {   /* 応答が届かなかったので同じ要求がもう一度来た */
      resend();
      continue;
    }
    last_seq = seq;
    last_type = type;
    {
      unsigned char *d = rbuf + 4;
      if (verbose && type != 'W' && type != 'R') {
        char t[3] = { type, ' ', 0 };
        put("rx: "); put(t); putnum(len); put("\r\n");
      }
      switch (type) {
      case 'H': {   /* "RX 版 最大長 空き(KB)" */
        static char hb[40];
        char *q = hb;
        const char *h0 = "RX " VERSION " 4096 ";
        long fr = freemem() / 1024;
        char nb[12], *np = nb + 11;
        while (*h0) *q++ = *h0++;
        *np = 0;
        do { *--np = '0' + fr % 10; fr /= 10; } while (fr);
        while (*np) *q++ = *np++;
        send('K', seq, hb, q - hb);
        break;
      }
      case 'O':
        if (wfd >= 0) { _dos_close(wfd); wfd = -1; }
        cstr(name, d + 1, len - 1, sizeof name);
        wfd = -1;
        if (d[0] == 1 && (wfd = _dos_open(name, 1)) >= 0)
          _dos_seek(wfd, 0, 2);
        else
          wfd = _dos_create(name, 0x20);
        if (wfd < 0) { put32(tmp, wfd); send('E', seq, tmp, 4); }
        else {
          put("rx: "); put(name); put("\r\n");
          send('K', seq, 0, 0);
        }
        break;
      case 'W':
        n = wfd < 0 ? -1 : _dos_write(wfd, (const char *)d, len);
        if (n != len) { put32(tmp, n); send('E', seq, tmp, 4); }
        else send('K', seq, 0, 0);
        break;
      case 'C':
        n = 0;
        if (wfd >= 0) { n = _dos_close(wfd); wfd = -1; }
        if (rfd >= 0) { _dos_close(rfd); rfd = -1; }
        if (n < 0) { put32(tmp, n); send('E', seq, tmp, 4); }
        else send('K', seq, 0, 0);
        break;
      case 'P':
        if (rfd >= 0) _dos_close(rfd);
        cstr(name, d, len, sizeof name);
        rfd = _dos_open(name, 0);
        if (rfd < 0) { put32(tmp, rfd); send('E', seq, tmp, 4); break; }
        n = _dos_seek(rfd, 0, 2);
        _dos_seek(rfd, 0, 0);
        put32(tmp, n);
        send('K', seq, tmp, 4);
        break;
      case 'R':
        n = (d[0] << 8) | d[1];
        if (n > MAXLEN) n = MAXLEN;
        n = rfd < 0 ? -1 : _dos_read(rfd, (char *)rbuf, n);
        if (n < 0) { put32(tmp, n); send('E', seq, tmp, 4); }
        else send('K', seq, rbuf, n);
        break;
      case 'X':
        if (verbose) { cstr(name, d, len, sizeof name); put("rx: > "); put(name); put("\r\n"); }
        n = run((const char *)d, len);
        put32(rbuf, n);
        __builtin_memcpy(rbuf + 4, outname, sizeof outname - 1);
        send('K', seq, rbuf, 4 + sizeof outname - 1);
        break;
      case 'S':   /* 統計: 受信バッファの最大の溜まり (4)、壊れた枠の数 (4) */
        put32(tmp, maxlof);
        put32(tmp + 4, nbad);
        maxlof = 0;
        send('K', seq, tmp, 8);
        break;
      case 'B':
        send('K', seq, 0, 0);
        n = now();
        while (since(n) < 20) ;   /* 応答を送り終えるまで待つ */
        _iocs_set232c((d[0] << 8) | d[1]);
        break;
      case 'Q':
        send('K', seq, 0, 0);
        n = now();
        while (since(n) < 10) ;
        put("rx: bye\r\n");
        return 0;
      default:
        send('E', seq, 0, 0);
      }
    }
  }
}

static int speed_code(long bps)
{
  static const long tab[] = { 75, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400 };
  int i;
  for (i = 0; i < 10; i++)
    if (tab[i] == bps)
      return i;
  return -1;
}

int rx_main(const unsigned char *cmd)
{
  int len = cmd[0], i = 1, server = 1, setspeed = 0, code = 0, keywait = -1;
  char drv[2];

  crcinit();
  while (i <= len) {
    while (i <= len && cmd[i] == ' ') i++;
    if (i > len) break;
    if (cmd[i] == '-' && i + 1 <= len) {
      int o = cmd[i + 1] | 0x20;
      i += 2;
      if (o == 'v') verbose = 1;
      else if (o == 's') { server = 2; }
      else if (o == 'm') { put("rx: free "); putnum(freemem() / 1024); put(" KB\r\n"); if (server == 1) server = 0; }
      else if (o == 'k') {
        keywait = 0;
        while (i <= len && cmd[i] >= '0' && cmd[i] <= '9') keywait = keywait * 10 + cmd[i++] - '0';
        if (server == 1) server = 0;
      }
      else if (o == 'b') {
        long v = 0;
        while (i <= len && cmd[i] >= '0' && cmd[i] <= '9') v = v * 10 + cmd[i++] - '0';
        if ((code = speed_code(v)) < 0) { put("rx: bad speed\r\n"); return 1; }
        setspeed = 1;
        if (server == 1) server = 0;
      } else {
        put("usage: rx [-b19200] [-k2] [-m] [-s] [-v]\r\n");
        return 1;
      }
    } else
      i++;
  }
  if (setspeed)
    _iocs_set232c(0x4c00 | code);   /* 1 stop, parity なし, 8bit, フロー制御なし */
  else if (server) {
    /* 2 進のデータを通すため、速さはそのままで XON/XOFF をやめる (SRAM の既定 4E07 は XON/XOFF あり) */
    int m = _iocs_set232c(0x4c07) & 0xffff;   /* 戻り値は前のモード。その速さに戻す */
    _iocs_set232c(0x4c00 | (m & 0x0f));
  }
  if (keywait >= 0) {
    long t = now();
    put("rx: CTTY AUX (RS-232C) -- press a key within ");
    putnum(keywait);
    put(" s to keep the keyboard\r\n");
    while (since(t) < keywait * 100L)
      if (_iocs_b_keysns()) {
        _iocs_b_keyinp();
        put("rx: keyboard kept\r\n");
        return 1;
      }
  }
  if (!server)
    return 0;
  drv[0] = 'A' + _dos_curdrv();
  outname[0] = drv[0];
  return serve();
}
