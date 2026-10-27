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
    .ascii  "\001CONTEE "         /* 先頭を 0x01 にする。"CONTEE  " だと contee.sys という名前のファイルを開くときにデバイスとして
                                   開かれ (CON や NUL と同じ)、書き込みが 「デバイスドライバに無効なコマンド」 になる */
enable:
    .byte   1                   /* 0 にすると写すのをやめる (ヘッダ +22。zmux.x が線を使い始めるときに 0 にする) */
    .even
/* 記録 (zmux.x -r が読む。ヘッダ +24 から): SCC ch.B の読める所 (RR0 RR1 RR10 RR12 RR13 RR15) を、組み込み時と、送る最初の 40 文字と、
   そのあと 2 の累乗番目の文字で控える。WR4 / WR11 / WR14 は読めない (Z8530 の書き込み専用)。1 件 12 バイト:
   +0 番号 (0xfffd = 組み込み直前、0xfffe = 組み込み直後)、+2 RR0、+3 RR1、+4 RR10、+5 RR12、+6 RR13、+7 RR15、+8 時刻 (1/100 秒の下位 16 ビット)、+10 予備 */
rec_tag:
    .ascii  "CTEREC01"
rec_n:
    .word   0                   /* 送った文字の数 (0xffff で止まる) */
rec_cnt:
    .word   0                   /* 控えた件数 */
rec_buf:
    .space  64*12

reqheader:
    .long   0
old_putc:
    .long   0
old_print:
    .long   0
old_putmes:
    .long   0

/* d1.w = 番号で SCC の読める所を控える (全レジスタを保つ) */
rec:
    movem.l %d0-%d3/%a0-%a1,%sp@-
    move.w  rec_cnt,%d2
    cmp.w   #64,%d2
    bcc.s   9f
    move.w  %d2,%d3
    mulu.w  #12,%d3
    lea     rec_buf,%a1
    adda.l  %d3,%a1
    move.w  %d1,%a1@
    moveq   #0x7f,%d0           /* _ONTIME */
    trap    #15
    move.w  %d0,%a1@(8)
    move.w  %sr,%sp@-
    ori.w   #0x0700,%sr
    lea     0xe98005,%a0
    move.b  %a0@,%a1@(2)        /* RR0 */
    move.b  #1,%a0@
    move.b  %a0@,%a1@(3)        /* RR1 */
    move.b  #10,%a0@
    move.b  %a0@,%a1@(4)        /* RR10 */
    move.b  #12,%a0@
    move.b  %a0@,%a1@(5)        /* RR12 = WR12 */
    move.b  #13,%a0@
    move.b  %a0@,%a1@(6)        /* RR13 = WR13 */
    move.b  #15,%a0@
    move.b  %a0@,%a1@(7)        /* RR15 = WR15 */
    move.w  %sp@+,%sr
    addq.w  #1,rec_cnt
9:  movem.l %sp@+,%d0-%d3/%a0-%a1
    rts

/* 送る文字を数えて、最初の 40 文字と 2 の累乗番目で控える */
rec_maybe:
    movem.l %d0-%d1,%sp@-
    move.w  rec_n,%d0
    cmp.w   #0xffff,%d0
    beq.s   8f
    addq.w  #1,%d0
    move.w  %d0,rec_n
    cmp.w   #40,%d0
    bls.s   1f
    move.w  %d0,%d1
    subq.w  #1,%d1
    and.w   %d0,%d1
    bne.s   8f
1:  move.w  %d0,%d1
    bsr.w   rec
8:  movem.l %sp@+,%d0-%d1
    rts

/* 1 文字を RS-232C に送る (d1.b)。d0/d1 以外は壊さない。enable が 0 なら送らない */
out1:
    tst.b   enable
    bne.s   0f
    rts
0:  move.l  %d0,%sp@-
    bsr.w   rec_maybe           /* 時定数を直す前の SCC を控える */
#ifdef TC0
    /* RSDRV.SYS は起動の途中で SCC を設定し直す (38400 に戻る)。送るたびに時定数を見て、0 でなければ 0 に戻す */
    move.l  %a0,%sp@-
    move.w  %sr,%sp@-
    ori.w   #0x0700,%sr
    lea     0xe98005,%a0
    move.b  #12,%a0@
    tst.b   %a0@
    beq.s   1f
    move.b  #12,%a0@
    clr.b   %a0@
    move.b  #13,%a0@
    clr.b   %a0@
1:  move.w  %sp@+,%sr
    move.l  %sp@+,%a0
#endif
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
    bsr.w   out1
    move.l  %sp@+,%d1
1:  bsr.w   out1
    movem.l %sp@+,%d0-%d1
    move.l  old_putc,%sp@-
    rts

/* _B_PRINT: a1 = 文字列 (0 で終わる) */
new_print:
    movem.l %d0-%d1/%a1,%sp@-
1:  move.b  %a1@+,%d1
    beq.s   2f
    bsr.w   out1
    bra.s   1b
2:  movem.l %sp@+,%d0-%d1/%a1
    move.l  old_print,%sp@-
    rts

/* _B_PUTMES: a1 = 文字列, d4.w = 長さ - 1 (位置を指定して描く。エラーのダイアログなど)。行ごとに改行を足す */
new_putmes:
    movem.l %d0-%d1/%d4/%a1,%sp@-
1:  move.b  %a1@+,%d1
    beq.s   2f
    bsr.w   out1
    dbra    %d4,1b
2:  moveq   #13,%d1
    bsr.w   out1
    moveq   #10,%d1
    bsr.w   out1
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
    bne.w   9f
    move.w  #0xfffd,%d1
    bsr.w   rec
    move.w  #0x4c09,%d1         /* 38400bps 8N1 */
    moveq   #0x30,%d0           /* _SET232C */
    trap    #15
#ifdef TC0
    /* XVI の zmux 用: SCC の時定数を 0 にして 78125bps (5MHz / 32 / 2) にする */
    move.w  %sr,%sp@-
    ori.w   #0x0700,%sr
    lea     0xe98005,%a0
    move.b  #12,%a0@
    clr.b   %a0@
    move.b  #13,%a0@
    clr.b   %a0@
    move.w  %sp@+,%sr
#endif
    move.w  #0xfffe,%d1
    bsr.w   rec
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
    bra.w   8f
9:  move.b  #3,%a5@(3)          /* 知らない命令 */
    move.b  #0x10,%a5@(4)
8:  movem.l %sp@+,%d0-%d7/%a0-%a6
    rts

init:
title:
    .ascii  "\r\ncontee.sys: screen output is copied to RS-232C\r\n\0"   /* as は -fexec-charset を見ないので ASCII で */
    .even
