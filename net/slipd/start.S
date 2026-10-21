/* slipd.x の入口 (C ライブラリを使わない)。Human68k は a0 = メモリ管理ポインタ, a1 = プログラムの終わりを渡す */
    .text
    .global _start
_start:
    move.l  %a0,mem_ptr
    move.l  %a1,prg_end
    move.l  %a2,cmd_line
    jsr     slipd_main
    move.w  %d0,%sp@-
    .short  0xff4c              /* DOS _EXIT2 */

/* long call_etd(void *entry, long cmd, void *arg): slip.sys の ETD 入口を d0 = 命令, a0 = 引数で呼ぶ */
    .global call_etd
call_etd:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

/* 簡易プロファイラ: 垂直同期の割り込み (約 60Hz) で、割り込まれた PC を zprof.buf に記録する。
 * zprof.on が 0 の間は何もしない。zprof.x で開始・取り出しをする ("SLPDPROF")。
 * _VDISPST で登録するので RTE で戻る */
    .global prof_vdisp
prof_vdisp:
    tst.l   zprof+8
    beq.s   1f
    movem.l %d0/%a0,%sp@-
    move.l  zprof+12,%d0
    andi.l  #4095,%d0
    lsl.l   #2,%d0
    lea     zprof+16,%a0
    move.l  %sp@(10),%a0@(0,%d0.l)  /* 8 バイト積んだ後ろの SR (2) の次が PC */
    addq.l  #1,zprof+12
    movem.l %sp@+,%d0/%a0
1:  rte

/* IOCS _SCSIDRV ($F5) の入口に挟むフック。slipd.x が SCSI のメールボックス (0xEA0060) を直に使っている間は、
 * ほかの SCSI の呼び出し (B: の scsiremote.sys など) を、その命令が終わるまで待たせる。ROM の読み書きの入口は
 * メールボックスが使用中かどうかを見ずに書き換えるので、待たせないと互いの命令を壊す。
 * ROM を呼んでいる間は scsi_inrom を数え、slipd.x はそれが 0 のときだけ命令を出す */
    .global scsi_hook, scsi_inrom, scsi_old
scsi_hook:
    addq.w  #1,scsi_inrom
1:  tst.b   0xea0060
    bmi.s   1b
    pea     2f
    move.l  scsi_old,%sp@-
    rts
2:  subq.w  #1,scsi_inrom
    rts

    .data
    .even
    .global zprof
zprof:
    .ascii  "SLPDPROF"
    .long   0                   /* +8  on */
    .long   0                   /* +12 pos */
    .space  4096*4              /* +16 PC */

    .bss
    .even
    .global mem_ptr, prg_end, cmd_line
scsi_old:   .long 0
scsi_inrom: .short 0
    .even
mem_ptr:  .long 0
prg_end:  .long 0
cmd_line: .long 0
