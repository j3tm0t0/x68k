/*
 * main.c -- tis.x の起動処理: 常駐・ソケット表の表示 (-s)・常駐の解除 (-r)
 */
#define __DOS_INLINE__
#include <x68k/dos.h>
#include "be.h"

extern char *mem_ptr;
extern unsigned char *cmd_line;
extern char _end;
extern long call_ti(void *entry, long cmd, void *arg);
void tis_init(void);

static int memeq(const void *a, const void *b, int n)
{
  const uint8_t *x = a, *y = b;
  while (n--)
    if (*x++ != *y++)
      return 0;
  return 1;
}

static void putn(unsigned long v)
{
  char d[12];
  int k = 0;
  do d[k++] = '0' + v % 10; while ((v /= 10) > 0);
  while (k > 0)
    _dos_putchar(d[--k]);
}

static void pad(const char *s, int w)
{
  int n = 0;
  for (; s[n]; n++)
    ;
  _dos_print(s);
  while (n++ < w)
    _dos_putchar(' ');
}

static const char *parse_ip(const char *p, uint32_t *ip)
{
  uint32_t a = 0;
  int k;
  for (k = 0; k < 4; k++) {
    unsigned v = 0, nd = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p++ - '0');
      nd++;
    }
    if (!nd || v > 255 || (k < 3 && *p++ != '.'))
      return 0;
    a = a << 8 | v;
  }
  *ip = a;
  return p;
}

/* npd.sys の ETD 入口 (npd.x と同じ探し方: "NUL" から始まるデバイスチェインで "/dev/ep0" + "EthDNPD ")。
   スーパーバイザで呼ぶ */
static void *find_npd(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "/dev/ep0", 8) && memeq(d + 22, "EthDNPD ", 8))
          return d + 30;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* tis.x -p の入口を npd.sys に預ける / 外す (npd.x が交換のたびに取りに来る)。戻り値: 0 / -1 (npd.sys 1.2 が無い) */
static int npd_register(void *entry)
{
  int ssp = _dos_super(0), r = -1;
  void *etd = find_npd();
  if (etd) {
    call_ti(etd, NPD_ETD_TSP_SET, entry);
    r = call_ti(etd, NPD_ETD_TSP_GET, 0) == (long)entry ? 0 : -1;
  }
  if (ssp > 0)
    _dos_super(ssp);
  return r;
}

/* 自分より前の常駐ブロックから "TCP/IP" を探す (libsocket と同じ探し方)。*tis = 1 なら tis.x、0 なら xip.x など */
static struct dos_mep *find_stack(int *tis)
{
  struct dos_mep *m = (struct dos_mep *)mem_ptr;
  int ssp = _dos_super(0);
  while ((m = m->prev_mp) != 0) {
    char *prog = (char *)m + 0x100;
    if (((uint32_t)m->parent_mp & 0xff000000) == 0xff000000 && memeq(prog, "TCP/IP", 6)) {
      *tis = memeq(prog + 12, "TISTACK1", 8);
      break;
    }
  }
  if (ssp > 0)
    _dos_super(ssp);
  return m;
}

static int show(struct tis_info *in)
{
  uint32_t i;
  char b[24];
  _dos_print("tis.x: backend ");
  _dos_print(in->be_name);
  _dos_print(", buffer ");
  putn(in->bufsz);
  _dos_print(" bytes x ");
  putn(in->nsock);
  if (in->own_addr) {
    fmt_addr(b, in->own_addr, 0)[-2] = 0;   /* ":0" を落とす */
    _dos_print(", own ");
    _dos_print(b);
  }
  _dos_print("\r\n  calls ");
  putn(in->calls);
  _dos_print(" waits ");
  putn(in->waits);
  _dos_print(" accepted ");
  putn(in->conns);
  _dos_print(" read ");
  putn(in->rbytes);
  _dos_print(" written ");
  putn(in->wbytes);
  _dos_print("\r\n");  if (in->pstat) {
    struct pico_stat *q = in->pstat;
    _dos_print("  pico xchg ");
    putn(q->xchg);
    _dos_print(" resend ");
    putn(q->resend);
    _dos_print(" stale ");
    putn(q->stale_rec);
    _dos_print(" netdown ");
    putn(q->netdown);
    _dos_print(" stall ");
    putn(q->stall);
    _dos_print("\r\n");
  }
  for (i = 0; i < in->nsock; i++) {
    struct tsock *t = &in->ts[i];
    if (t->st == S_FREE)
      continue;
    _dos_print("  ");
    putn(i + SOCKBASE);
    _dos_print(t->dgram ? " udp " : " tcp ");
    pad(state_str(t), 13);
    fmt_addr(b, t->laddr, t->lport);
    pad(b, 22);
    fmt_addr(b, t->raddr, t->rport);
    pad(b, 22);
    _dos_print("rx ");
    putn(t->rx.len);
    _dos_print(" ref ");
    putn(in->usock[i].refcnt);
    if (in->usock[i].noblock)
      _dos_print(" nonblock");
    _dos_print("\r\n");
  }
  return 0;
}

/* 命令番号 24 (_TI_socket) からの名前 */
static const char *const cmdname[] = {
  "socket", "bind", "listen", "accept", "connect", "read_s", "write_s", "recvfrom", "sendto", "close_s",
  "socklen", "getsockname", "getpeername", "sockkick", "shutdown", "usesock", "recvline", "sendline",
  "rrecvchar", "recvchar", "usflush", "seteol", "sockmode", "setflush", "psocket", "sockerr", "sockstate",
};

static int show_trace(struct tis_info *in)
{
  uint32_t n = in->tpos < TRACE ? in->tpos : TRACE, k;
  for (k = in->tpos - n; k != in->tpos; k++) {
    int e = k % TRACE, c = in->trace[e].cmd - 24;
    putn(k);
    _dos_putchar(' ');
    pad(c >= 0 && c < (int)(sizeof cmdname / sizeof cmdname[0]) ? cmdname[c] : "?", 12);
    putn(in->trace[e].fd);
    _dos_print(" len ");
    putn(in->trace[e].len);
    _dos_print(" -> ");
    if (in->trace[e].ret < 0) {
      _dos_putchar('-');
      putn(-in->trace[e].ret);
    } else
      putn(in->trace[e].ret);
    _dos_print("\r\n");
  }
  return 0;
}

int tis_main(void)
{
  const char *p = (const char *)cmd_line + 1;
  uint32_t own = 0, kb = 8, txkb = 4;
  int trace = 0, pico = 0;
  int status = 0, remove = 0, force = 0, tis = 0;
  struct dos_mep *m;

  while (*p) {
    if (*p == ' ' || *p == '\t') {
      p++;
      continue;
    }
    if (p[0] == '-' && p[1] == 's') {
      status = 1;
      p += 2;
    } else if (p[0] == '-' && p[1] == 't') {
      status = trace = 1;
      p += 2;
    } else if (p[0] == '-' && p[1] == 'r') {
      remove = 1;
      p += 2;
      if (*p == 'f') {
        force = 1;
        p++;
      }
    } else if (p[0] == '-' && p[1] == 'p') {
      /* TCP / UDP / DNS を Pico に任せる (npd.x の交換に TSP レコードを載せる) */
      pico = 1;
      p += 2;
    } else if (p[0] == '-' && p[1] == 'a') {
      p += 2;
      while (*p == ' ')
        p++;
      if (!(p = parse_ip(p, &own)))
        goto usage;
    } else if (p[0] == '-' && p[1] == 'b') {
      p += 2;
      while (*p == ' ')
        p++;
      kb = 0;
      while (*p >= '0' && *p <= '9')
        kb = kb * 10 + (*p++ - '0');
      if (kb < 1 || kb > 64)
        goto usage;
    } else {
    usage:
      _dos_print("usage: tis.x [-p] [-a a.b.c.d] [-b KB(1-64)] | -s | -r[f]\r\n");
      return 1;
    }
  }

  m = find_stack(&tis);
  if (status || remove) {
    struct tis_info *in;
    uint32_t i, open = 0;
    if (!m || !tis) {
      _dos_print(m ? "tis.x: 常駐しているのは tis.x ではありません (xip.x?)\r\n" : "tis.x: 常駐していません\r\n");
      return 1;
    }
    in = (struct tis_info *)call_ti((char *)m + 0x100 + 8, TIS_INFO, 0);
    if (trace)
      return show_trace(in);
    if (status)
      return show(in);
    for (i = 0; i < in->nsock; i++)
      if (in->ts[i].st != S_FREE)
        open++;
    if (open && !force) {
      _dos_print("tis.x: ソケットが開いています (-rf で強制的に解除)\r\n");
      return 1;
    }
    if (in->be_name[0] == 'p')
      npd_register(0);                  /* npd.x が解放した入口を呼ばないように (Pico は次の zepoch で前のソケットを捨てる) */
    if (_dos_mfree((char *)m + 16) < 0) {
      _dos_print("tis.x: 解除できません\r\n");
      return 1;
    }
    _dos_print("tis.x: 常駐を解除しました\r\n");
    return 0;
  }

  if (m) {
    _dos_print(tis ? "tis.x: すでに常駐しています\r\n" : "tis.x: ほかの TCP/IP (xip.x?) が常駐しています\r\n");
    return 1;
  }
  {
    /* 受信リングは BSS の後ろ。_KEEPPR の大きさに含める */
    uint8_t *pool = (uint8_t *)(((uint32_t)&_end + 3) & ~3);
    uint32_t bs = kb * 1024, txs = pico ? txkb * 1024 : 0, i;
    char *blk_end = ((struct dos_mep *)mem_ptr)->block_end;
    if (pool + NSOCK * (bs + txs) > (uint8_t *)blk_end) {
      _dos_print("tis.x: メモリが足りません\r\n");
      return 1;
    }
    tis_init();
    for (i = 0; i < NSOCK; i++) {
      ts[i].rx.buf = pool + i * bs;
      ts[i].rx.size = bs;
    }
    info.bufsz = bs;
    info.own_addr = own;
    if (pico) {
      extern char ti_entry;
      /* 送信リングは受信リングの後ろ。番号と epoch は時刻から (Z のリセットで前の回と重ならないように) */
      pico_init(pool + NSOCK * bs, txs, ((uint32_t)_dos_getdate() << 16) ^ (uint32_t)_dos_gettim2());
      be = &be_pico;
      info.be_name = "pico";
      info.pstat = &pstat;
      /* 表を用意し終えてから入口を預ける (預けた瞬間から npd.x のスレッドが呼ぶ) */
      if (npd_register(&ti_entry) < 0) {
        _dos_print("tis.x: npd.sys 1.2 が組み込まれていません (-p には npd.sys と npd.x が要る)\r\n");
        return 1;
      }
    }
    _dos_print(pico ? "tis.x: TCP/IP via Pico (npd.x), loopback 127.0.0.0/8" : "tis.x: TCP/IP loopback stack (127.0.0.0/8");
    if (own) {
      char b[24];
      fmt_addr(b, own, 0)[-2] = 0;
      _dos_print(", ");
      _dos_print(b);
    }
    _dos_print("), ");
    putn(kb);
    _dos_print("KB x 32 sockets\r\n");
    _dos_keeppr((char *)pool + NSOCK * (bs + txs) - (mem_ptr + 0x100), 0);
  }
  return 0;
}
