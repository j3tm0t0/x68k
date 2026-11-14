/*
 * x68rd_proto.h -- Human68k remote drive over SCSI (BlueSCSI SCSI ID6, "X68NP" target)
 *
 * Same file in j3tm0t0/x68k net/bsrmt/ (driver bsrmt.sys, harness/picosim) and in the BlueSCSI firmware
 * (lib/X68kRemote). MIT licence (j3tm0t0), so that both can carry it.
 *
 * The X68000 writes one request to LBA X68RD_WLBA and reads the answer from LBA X68RD_RLBA, with the
 * 10 byte READ / WRITE commands (_S_WRITEEXT / _S_READEXT). All values big endian.
 *
 * Request (1 to 32 sectors):
 *   +0  "X6RD"
 *   +4  seq (32)      one more for every new request, never 0
 *   +8  len (16)      bytes of the command at +16 (for WRITE: the 11 byte head; the data is at +512)
 *   +10 flags (16)    X68RD_F_RETRY: the same request again (same seq)
 *   +12 time, date    the clock of the X68000 (DOS format), used for the time stamps of written files
 *   +16 command       x68kremote.h form: first byte = command | unit << 5
 *   +512 data         WRITE only, up to X68RD_DATASIZE bytes
 *
 * Answer (the X68000 reads as many sectors as the command can need):
 *   +0  "X6RA"
 *   +4  seq           of the request
 *   +8  len (16)      bytes of the result at +16 (READ: 2, the data is at +512)
 *   +10 status (16)   X68RD_ST_*
 *   +12 spare (32)
 *   +16 result        x68kremote.h form
 *   +512 data         READ only
 *
 * Changes from x68kremote.h (yunkya2/x68kserremote): FILES / NFILES carry a count (up to X68RD_NFILES
 * entries of X68RD_FILESINFO bytes per answer), INIT answers with the version and the sizes, and the data
 * of READ / WRITE is at +512 instead of right after the head.
 */
#ifndef X68RD_PROTO_H
#define X68RD_PROTO_H

#define X68RD_WLBA          0x200000
#define X68RD_RLBA          0x210000
#define X68RD_SECTORS       32                  /* largest request / answer */
#define X68RD_BYTES         (X68RD_SECTORS * 512)
#define X68RD_HDR           16
#define X68RD_DATA_OFF      512
#define X68RD_DATASIZE      (X68RD_BYTES - X68RD_DATA_OFF)   /* 15872 */
#define X68RD_NFILES        32
#define X68RD_FILESINFO     32                  /* atr 1, time 2, date 2, size 4, name 23 */
#define X68RD_NAMEBUF       88                  /* dos_namebuf: flag 1, drive 1, path 65, name1 8, ext 3, name2 10 */
#define X68RD_VERSION       1

#define X68RD_REQ_MAGIC     0x58365244          /* "X6RD" */
#define X68RD_ANS_MAGIC     0x58365241          /* "X6RA" */
#define X68RD_F_RETRY       0x0001

#define X68RD_ST_OK         0
#define X68RD_ST_NOREQ      1                   /* no request since the start / the last bus reset */
#define X68RD_ST_BADHDR     2
#define X68RD_ST_DISABLED   3                   /* drive off, USB mass storage, no SD card */
#define X68RD_ST_BUSY       4                   /* still working on it, read again */

/* Commands (low 5 bits | 0x40; the top 3 bits of the first byte are the unit) and their layouts.
 * Offsets are from the start of the command (request +16) / result (answer +16). */
#define X68RD_INIT          0x40    /* cmd: [c]                          res: [res][version][nfiles][0][datasize 16][units] */
#define X68RD_CHDIR         0x41    /* cmd: [c][namebuf]                 res: [res] */
#define X68RD_MKDIR         0x42
#define X68RD_RMDIR         0x43
#define X68RD_RENAME        0x44    /* cmd: [c][namebuf old][namebuf new] res: [res] */
#define X68RD_DELETE        0x45
#define X68RD_CHMOD         0x46    /* cmd: [c][attr][namebuf]           res: [res] (attribute when attr was 0xff) */
#define X68RD_FILES         0x47    /* cmd: [c][attr][num][filep 32][namebuf]   res: [res][num][info x num] */
#define X68RD_NFILES_CMD    0x48    /* cmd: [c][num][filep 32]                  res: [res][num][info x num] */
#define X68RD_CREATE        0x49    /* cmd: [c][attr][mode][fcb 32][namebuf]    res: [res] */
#define X68RD_OPEN          0x4a    /* cmd: [c][mode][fcb 32][namebuf]          res: [res][size 32] */
#define X68RD_CLOSE         0x4b    /* cmd: [c][fcb 32]                         res: [res] */
#define X68RD_READ          0x4c    /* cmd: [c][fcb 32][pos 32][len 16]         res: [len 16], data at +512 */
#define X68RD_WRITE         0x4d    /* cmd: [c][fcb 32][pos 32][len 16], data at +512   res: [len 16] */
#define X68RD_FILEDATE      0x4f    /* cmd: [c][fcb 32][time 16][date 16]       res: [time 16][date 16] */
#define X68RD_DSKFRE        0x50    /* cmd: [c]                          res: [res 32][free 16][total 16][clusect 16][sectsize 16] */

#endif
