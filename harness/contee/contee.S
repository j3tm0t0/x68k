/*
 * contee.sys -- 画面に出す文字 (IOCS _B_PUTC / _B_PRINT) を RS-232C にも流す (PROFILE=dev 用)
 *
 * CONFIG.SYS の IOCS.X の後ろで組み込むと、それ以降のデバイスドライバの組み込みメッセージや、
 * 画面で止まったときの表示を、シリアルのログで見られる (COMMAND.X の CTTY AUX より前の分)。
 * IOCS.X は _B_PUTC などを自分の版に置き換えるので、それより前に組み込むと途中で外れる。
 * 組み込み時に RS-232C を 38400bps 8N1 (rsspeed.x 4c09 と同じ) にする。
 */
    .text
devheader:
    .long   -1                  /* 次のデバイス */
    .word   0x8000              /* キャラクタデバイス */
    .long   strategy
    .long   interrupt
    .ascii  "CONTEE  "

reqheader:
    .long   0
old_putc:
    .long   0
old_print:
    .long   0
old_putmes:
    .long   0

/* 1 文字を RS-232C に送る (d1.b)。d0/d1 以外は壊さない */
out1:
    move.l  %d0,%sp@-
    moveq   #0x35,%d0           /* _OUT232C */
    trap    #15
    move.l  %sp@+,%d0
    rts

/* _B_PUTC: d1.w = 文字 (2 バイト文字は上位バイトが先) */
new_putc:
    movem.l %d0-%d1,%sp@-
    move.w  %d1,%d0
    lsr.w   #8,%d0
    beq.s   1f
    move.l  %d1,%sp@-
    move.b  %d0,%d1
    bsr.s   out1
    move.l  %sp@+,%d1
1:  bsr.s   out1
    movem.l %sp@+,%d0-%d1
    move.l  old_putc,%sp@-
    rts

/* _B_PRINT: a1 = 文字列 (0 で終わる) */
new_print:
    movem.l %d0-%d1/%a1,%sp@-
1:  move.b  %a1@+,%d1
    beq.s   2f
    bsr.s   out1
    bra.s   1b
2:  movem.l %sp@+,%d0-%d1/%a1
    move.l  old_print,%sp@-
    rts

/* _B_PUTMES: a1 = 文字列, d4.w = 長さ - 1 (位置を指定して描く。エラーのダイアログなど)。行ごとに改行を足す */
new_putmes:
    movem.l %d0-%d1/%d4/%a1,%sp@-
1:  move.b  %a1@+,%d1
    beq.s   2f
    bsr.s   out1
    dbra    %d4,1b
2:  moveq   #13,%d1
    bsr.s   out1
    moveq   #10,%d1
    bsr.s   out1
    movem.l %sp@+,%d0-%d1/%d4/%a1
    move.l  old_putmes,%sp@-
    rts

strategy:
    move.l  %a5,reqheader
    rts

interrupt:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    movea.l reqheader,%a5
    tst.b   %a5@(2)             /* 命令 0 = 初期化 */
    bne.s   9f
    move.w  #0x4c09,%d1         /* 38400bps 8N1 */
    moveq   #0x30,%d0           /* _SET232C */
    trap    #15
    move.w  #0x120,%d1          /* _B_PUTC */
    lea     new_putc,%a1
    move.l  #0x80,%d0           /* _B_INTVCS */
    trap    #15
    move.l  %d0,old_putc
    move.w  #0x121,%d1          /* _B_PRINT */
    lea     new_print,%a1
    move.l  #0x80,%d0
    trap    #15
    move.l  %d0,old_print
    move.w  #0x12f,%d1          /* _B_PUTMES */
    lea     new_putmes,%a1
    move.l  #0x80,%d0
    trap    #15
    move.l  %d0,old_putmes
    pea     title
    .short  0xff09              /* DOS _PRINT */
    addq.l  #4,%sp
    move.l  #init,%a5@(14)      /* ここから後ろは常駐しない */
    clr.b   %a5@(3)
    clr.b   %a5@(4)
    bra.s   8f
9:  move.b  #3,%a5@(3)          /* 知らない命令 */
    move.b  #0x10,%a5@(4)
8:  movem.l %sp@+,%d0-%d7/%a0-%a6
    rts

init:
title:
    .ascii  "\r\ncontee.sys: screen output is copied to RS-232C (38400bps)\r\n\0"   /* as は -fexec-charset を見ないので ASCII で */
    .even
