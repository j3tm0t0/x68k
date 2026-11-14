/*
 * x68zl_proto.h -- zmux の線を RS-232C の代わりに BlueSCSI の SCSI ID6 に通す形 (net/zmux/DESIGN.md §9)
 *
 * MIT License. Copyright (c) 2026 j3tm0t0.
 * zmux.sys (X68000)、harness/picosim/zl.c (XEiJ の試験)、BlueSCSI のファーム (手元のブランチ) で同じものを使う。
 *
 * BlueSCSI はバイトの列をそのまま TCP に渡すだけで、zmux のフレーム・CRC・送り直しは今と同じく zmux.sys と zmuxd が持つ。
 * X68000 は ID6 (X68NP) に次の 2 つを繰り返す:
 *
 *   READ  X68ZL_RLBA, n セクタ (1〜X68ZL_SECTORS):
 *     +0  "X6ZR"
 *     +4  len (16)      この後ろ (+16) に続く、TCP から来たバイトの数 (n * 512 - 16 まで)
 *     +6  txfree (16)   今 WRITE で受け取れるバイトの数 (これを超えて書いた分は捨てる)
 *     +8  rxmore (16)   この READ の後にまだ残っているバイトの数 (65535 で頭打ち)
 *     +10 state (8)     bit0 = TCP がつながっている、bit1 = 有効 (0 なら ini で切ってある)
 *     +11 reserved (5)
 *     +16 data
 *     読んだバイトは BlueSCSI から消える (SCSI のエラーで X68000 に届かなければ失われる。zmux の送り直しで戻る)。
 *
 *   WRITE X68ZL_WLBA, n セクタ:
 *     +0  "X6ZW"
 *     +4  len (16)      +16 から続くバイトの数 (n * 512 - 16 まで)
 *     +6  flags (16)    bit0 = X68000 が始まったところ (BlueSCSI は送りかけのバイトを捨てる)
 *     +8  seq (16)      WRITE の番号。SCSI の終わりで失敗した WRITE は同じ番号・同じ中身で送り直し、BlueSCSI は
 *                       直前に受け取ったものと同じ番号なら捨てる (届いていたのに失敗に見えた WRITE を二重に渡さない。
 *                       素の文字はフレームの番号で守られないので)。X68000 は始めの番号を時計から取る (起動し直したときに
 *                       前の番号と重なりにくくする)。番号の比較は F_START より先
 *     +10 reserved (6)
 *     +16 data
 *     TCP がつながっていなければ捨てる (線の先に誰もいないシリアルと同じ)。
 *
 * 数はすべてビッグエンディアン。TCP は BlueSCSI が待ち受け (既定 6871)、zmuxd が --tcp HOST:6871 でつなぐ。
 * つながるのは 1 本で、新しい接続が来たら古いものを切る。
 */
#ifndef X68ZL_PROTO_H
#define X68ZL_PROTO_H

#define X68ZL_WLBA      0x220000
#define X68ZL_RLBA      0x230000
#define X68ZL_SECTORS   8
#define X68ZL_HDR       16
#define X68ZL_MAXDATA   (X68ZL_SECTORS * 512 - X68ZL_HDR)
#define X68ZL_PORT      6871

#define X68ZL_RMAGIC    0x58365a52      /* "X6ZR" */
#define X68ZL_WMAGIC    0x58365a57      /* "X6ZW" */

#define X68ZL_ST_CONN   0x01
#define X68ZL_ST_ON     0x02

#define X68ZL_F_START   0x0001

#endif
