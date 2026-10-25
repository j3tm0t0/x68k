/*
 * zrmt.sys -- リモートドライブ (チャネル 2) のデバイスヘッダと、アセンブラで書く入口 (net/zmux/DESIGN.md)
 *
 * Human68k のリモートドライブ (属性 0x2000)。要求は zrmt.c の rd_interrupt が受け、zmux.sys の入口 (命令 6) で Pi と交換する。
 * ヘッダの形は yunkya2 氏の x68kserremote (MIT) の driver/head.S と同じ。
 */
    .text
    .global devheader
devheader:
    .long   -1                  /* +0x00 次のデバイス */
    .word   0x2000              /* +0x04 属性: リモートドライブ */
    .long   strategy            /* +0x06 */
    .long   interrupt_asm       /* +0x0a */
    .ascii  "\001ZMUXDRV"       /* +0x0e デバイス名 */

strategy:
    move.l  %a5,reqheader
    rts

interrupt_asm:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  reqheader,%sp@-
    jsr     rd_interrupt
    addq.l  #4,%sp
    movea.l reqheader,%a5
    move.b  %d0,%a5@(3)         /* エラーコード下位 */
    lsr.w   #8,%d0
    move.b  %d0,%a5@(4)         /* エラーコード上位 */
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rts

/* long call_entry(void *entry, long cmd, long *args): zmux.sys の入口を d0 = 命令, a0 = 引数で呼ぶ */
    .global call_entry
call_entry:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

    .global dos_print, dos_putchar
dos_print:
    move.l  %sp@(4),%sp@-
    .short  0xff09              /* DOS _PRINT */
    addq.l  #4,%sp
    rts
dos_putchar:
    move.w  %sp@(6),%sp@-
    .short  0xff02              /* DOS _PUTCHAR */
    addq.l  #2,%sp
    rts

    .data
    .even
reqheader:
    .long   0
