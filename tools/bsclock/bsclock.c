/*
 * bsclock.x -- BlueSCSI v2 (Pico 2 W、ファームのブランチ x68k-ota) が SNTP で得た時刻を、X68000 の時計 (RTC) に設定する。
 *
 *   bsclock          SCSI ID 6 の BlueSCSI に時刻を聞き、IOCS _DATESET / _TIMESET で設定する
 *   bsclock -n       聞いて表示するだけ (設定しない)
 *   bsclock -r       X68000 の時計を表示するだけ (BlueSCSI には聞かない)
 *   bsclock -i ID    netpipe の装置の SCSI ID (既定 6)
 *
 * 時刻は netpipe の装置 (INQUIRY の製品名が "X68NP" で始まる SCSI ID、BlueSCSI の bluescsi.ini の X68kNetpipe=1) に、
 * 起動の制御のレコード [0xA8][4] (TIME) を 1 回の交換 (LBA 0x100000 に書き、0x110000-0x1ffff7 から読む) で聞く。答えは
 *   [0xA8][4][status][年 2][月][日][時][分][秒][曜日 0=日][UTC との差 (15 分単位)][1970 年からの秒 4]
 * status 7 は「まだ時刻が無い」(BlueSCSI が起動してから SNTP の答えをまだ受け取っていない)。時差は BlueSCSI の
 * bluescsi.ini の X68kTZ (既定 +9) で、答えの日時はその時差の地方時。
 *
 * 終了コード: 0 = 設定した (-n / -r は表示した)、1 = netpipe の装置が無い、2 = まだ時刻が無い、3 = 失敗
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __IOCS_INLINE__
#include <x68k/iocs.h>
#define __DOS_INLINE__
#include <x68k/dos.h>

#define NP_WLBA     0x100000
#define NP_RLBA     0x110000
#define BOOT_MARK   0xa8
#define OP_TIME     4
#define ST_NOTIME   7

static unsigned char blk[512] __attribute__((aligned(4)));
static int np_id = 6;

/* INQUIRY の製品名が "X68NP" のときだけ netpipe の装置とする。普通のディスクの LBA 0x100000 に書くと中身を壊す */
static int np_probe(void)
{
  int i;
  for (i = 0; i < 20; i++) {
    int r = _iocs_s_testunit(np_id);
    if (r == 0)
      break;
    if (r != 2 && r != 8)
      return -1;
    _iocs_s_request(8, np_id, blk);
  }
  memset(blk, 0, 36);
  if (_iocs_s_inquiry(36, np_id, (struct iocs_inquiry *)blk) != 0)
    return -1;
  return memcmp(blk + 16, "X68NP", 5) == 0 ? 0 : -1;
}

/* [0xA8][op] を送り、答え [0xA8][op][status]... を指して返す。len に答えの長さ。NULL = 答えが無い */
static const unsigned char *np_request(int op, int *len)
{
  static unsigned long seq;
  memset(blk, 0, sizeof blk);
  memcpy(blk, "X6NR", 4);
  seq += 0x01000193 ^ (unsigned long)_iocs_ontime().sec;
  blk[4] = seq >> 24; blk[5] = seq >> 16; blk[6] = seq >> 8; blk[7] = seq;
  blk[9] = 2 + 2;                       /* 送るレコードの合計: [長さ 2][中身 2] */
  blk[10] = 496 >> 8; blk[11] = 496 & 255;
  blk[17] = 2;
  blk[18] = BOOT_MARK; blk[19] = op;
  if (_iocs_s_writeext(NP_WLBA, 1, np_id, 1, blk) != 0)
    return NULL;
  /* 答えは 0x110000-0x1ffff7 のどこを読んでも同じ。読む位置を毎回変える (X68000 Z の Pseudo SCSI は読んだセクタを
     キャッシュする。npd.x と同じ。XEiJ の picosim もそれをまねる) */
  if (_iocs_s_readext(NP_RLBA + (seq % 0x1dfff) * 8, 1, np_id, 1, blk) != 0)
    return NULL;
  /* 答え: 見出し 16 (同じ seq) + [長さ 2][空き 14][0xA8][op][status][値...] */
  if (memcmp(blk, "X6NR", 4) != 0 || blk[4] != (unsigned char)(seq >> 24) || blk[7] != (unsigned char)seq ||
      blk[16] != 0 || blk[17] < 3 || blk[32] != BOOT_MARK || blk[33] != op)
    return NULL;
  *len = blk[17];
  return blk + 32;
}

static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

static const char wname[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* X68000 の時計: IOCS _DATEGET / _TIMEGET (BCD) と、Human68k の DOS _GETDATE (曜日も) */
static void show_rtc(const char *label)
{
  int d = _iocs_bindateget(), t = _iocs_timeget(), g = _dos_getdate();
  printf("%s%04d-%02d-%02d %02d:%02d:%02d %s (DOS %04d-%02d-%02d %s)\n", label, 1980 + unbcd((d >> 16) & 0xff),
         unbcd((d >> 8) & 0xff), unbcd(d & 0xff), unbcd((t >> 16) & 0xff), unbcd((t >> 8) & 0xff), unbcd(t & 0xff),
         wname[((d >> 24) & 7) % 7], 1980 + ((g >> 9) & 0x7f), (g >> 5) & 15, g & 31, wname[((g >> 16) & 7) % 7]);
}

static void usage(void)
{
  fprintf(stderr, "usage: bsclock [-n] [-r] [-i ID]\n"
          "  BlueSCSI (netpipe, SCSI ID 6) の時刻を X68000 の時計に設定する。-n: 表示だけ  -r: X68000 の時計を表示\n");
  exit(3);
}

int main(int argc, char **argv)
{
  int i, dry = 0, len = 0;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0)
      dry = 1;
    else if (strcmp(argv[i], "-r") == 0) {
      show_rtc("");
      return 0;
    } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      np_id = atoi(argv[++i]);
      if (np_id < 0 || np_id > 7)
        usage();
    } else
      usage();
  }

  if (np_probe() != 0) {
    fprintf(stderr, "bsclock: SCSI ID %d は netpipe の装置ではない (BlueSCSI の X68kNetpipe=1 が要る)\n", np_id);
    return 1;
  }
  const unsigned char *r = np_request(OP_TIME, &len);
  if (!r) {
    fprintf(stderr, "bsclock: SCSI ID %d から時刻の答えが無い\n", np_id);
    return 3;
  }
  if (r[2] == ST_NOTIME) {
    fprintf(stderr, "bsclock: BlueSCSI はまだ時刻を持っていない (Wi-Fi・SNTP の X68kNTP を確かめる。起動直後なら少し待つ)\n");
    return 2;
  }
  if (r[2] != 0 || len < 12) {
    fprintf(stderr, "bsclock: BlueSCSI が時刻の要求を受け付けなかった (status %d、ファームが古い?)\n", r[2]);
    return 3;
  }
  int year = r[3] << 8 | r[4], mon = r[5], day = r[6], hour = r[7], min = r[8], sec = r[9], wday = r[10];
  int q = (signed char)r[11], aq = q < 0 ? -q : q;
  if (year < 1980 || year > 2079 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59 ||
      wday > 6) {
    fprintf(stderr, "bsclock: 時刻が X68000 の時計の範囲 (1980-2079 年) の外: %04d-%02d-%02d %02d:%02d:%02d\n",
            year, mon, day, hour, min, sec);
    return 3;
  }
  printf("BlueSCSI: %04d-%02d-%02d %02d:%02d:%02d %s (UTC%c%d:%02d)\n", year, mon, day, hour, min, sec, wname[wday],
         q < 0 ? '-' : '+', aq / 4, aq % 4 * 15);
  if (dry)
    return 0;

  /* 時計 (RP5C15) に書く BCD は ROM の _DATEBCD / _TIMEBCD で作る。日付の BCD には曜日 (bit 27-24) と、うるう年の
     数え (年 % 4、bit 31-28) も入る。自分で組むとうるう年の数えが合わず、時刻も 12 時間制として読まれることがある */
  int dbcd = _iocs_bindatebcd(year << 16 | mon << 8 | day);
  int tbcd = _iocs_timebcd(hour << 16 | min << 8 | sec);
  if (getenv("BSCLOCK_DEBUG"))
    printf("debug: date %08x time %08x\n", dbcd, tbcd);
  if (dbcd == -1 || tbcd == -1) {
    fprintf(stderr, "bsclock: _DATEBCD / _TIMEBCD が受け付けない日時\n");
    return 3;
  }
  _iocs_bindateset(dbcd);
  _iocs_timeset(tbcd);
  show_rtc("X68000:   ");
  return 0;
}
