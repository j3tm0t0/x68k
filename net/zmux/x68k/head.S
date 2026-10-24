/*
 * zmux.sys -- デバイスヘッダと、アセンブラで書く入口 (net/zmux/DESIGN.md)
 *
 * 文字デバイス "ZCON" (CTTY ZCON でコンソールにする)。ヘッダの後ろ (+0x16) に "ZMUX" と、zmux.x のスレッドが
 * 呼ぶ入口 (+0x1a、d0 = 命令) を置く。zmux.x はデバイスチェインから "ZCON" + "ZMUX" を探す。
 */
    .text
    .global devheader
devheader:
    .long   -1                  /* +0x00 次のデバイス */
    .word   0xc000              /* +0x04 属性: 文字デバイス、IOCTRL 可 (RSDRV.SYS の AUX と同じ) */
    .long   strategy            /* +0x06 */
    .long   interrupt_asm       /* +0x0a */
    .ascii  "AUX     "          /* +0x0e デバイス名: RSDRV.SYS の AUX より先に見つかる */
    .ascii  "ZMUX"              /* +0x16 zmux.x が探す印 */
zmux_entry:                     /* +0x1a 入口: d0 = 命令 (0 = 線を回す、1 = 統計の場所) → d0 */
    movem.l %d1-%d7/%a0-%a6,%sp@-
    move.l  %d0,%sp@-
    jsr     zmux_call
    addq.l  #4,%sp
    movem.l %sp@+,%d1-%d7/%a0-%a6
    rts

strategy:
    move.l  %a5,reqheader
    rts

interrupt_asm:
    movem.l %d0-%d7/%a0-%a6,%sp@-
    move.l  reqheader,%sp@-
    jsr     dev_interrupt
    addq.l  #4,%sp
    movea.l reqheader,%a5
    move.b  %d0,%a5@(3)         /* エラーコード下位 */
    lsr.w   #8,%d0
    move.b  %d0,%a5@(4)         /* エラーコード上位 */
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rts

    .global dos_print
dos_print:
    move.l  %sp@(4),%sp@-
    .short  0xff09              /* DOS _PRINT */
    addq.l  #4,%sp
    rts

    .data           /* .bss は初期化で 0 にするので、初期化中に使う reqheader は .data に置く */
    .even
reqheader:
    .long   0
