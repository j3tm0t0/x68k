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
zmux_entry:                     /* +0x1a 入口: d0 = 命令, a0 = 引数 → d0 (命令は zmux.c の zmux_call) */
    movem.l %d1-%d7/%a0-%a6,%sp@-
    move.w  %sr,%d1
    btst    #13,%d1
    bne.s   1f
    /* ユーザーモード (zmux.x / npd.x のスレッド) から呼ばれたら、スーパーバイザに上がって回る。線の排他 (busy) を持ったまま
       タイマーでスレッドを切り替えられると、スーパーバイザで来た zrmt.sys / AUX の読み書きが busy を待ち続けて止まる
       (Human68k はスーパーバイザの間はスレッドを切り替えないので、持ち主に順番が回らない) */
    move.l  %a0,%sp@-
    move.l  %d0,%sp@-
    clr.l   %sp@-
    .short  0xff20              /* DOS _SUPER (0) */
    addq.l  #4,%sp
    move.l  %d0,%d7             /* 元のスーパーバイザスタック (0 以下ならもう上がっていた) */
    move.l  %sp@+,%d0
    movea.l %sp@+,%a0
    bsr.s   zmux_do
    move.l  %d0,%d6
    tst.l   %d7
    ble.s   0f
    move.l  %d7,%sp@-
    .short  0xff20              /* DOS _SUPER (ssp): ユーザーモードに戻る */
    addq.l  #4,%sp
0:  move.l  %d6,%d0
    bra.s   2f
1:  bsr.s   zmux_do
2:  movem.l %sp@+,%d1-%d7/%a0-%a6
    rts
zmux_do:
    move.l  %a0,%sp@-
    move.l  %d0,%sp@-
    jsr     zmux_call
    addq.l  #8,%sp
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

/* キー入力の注入 (チャネル 1): IOCS _B_KEYSNS ($01) / _B_KEYINP ($00) のフック。trap #15 の振り分けは
 * d0 = 番号で jsr してくる (d0 以外は呼んだ側のまま)。注入のキーがあればそれを返し、無ければ元の処理へ */
    .global hk_keysns, hk_keyinp, old_keysns, old_keyinp, call_orig
hk_keysns:
    movem.l %d1-%d7/%a0-%a6,%sp@-
    jsr     key_peek            /* d0 = 注入のキー (無ければ 0) */
    movem.l %sp@+,%d1-%d7/%a0-%a6
    tst.l   %d0
    bne.s   1f
    moveq   #1,%d0
    move.l  old_keysns,%sp@-
1:  rts
hk_keyinp:
    movem.l %d1-%d7/%a0-%a6,%sp@-
    jsr     key_get             /* 注入のキーか、本物のキーが来るまで待つ */
    movem.l %sp@+,%d1-%d7/%a0-%a6
    rts
/* long call_orig(void *entry, long num): 元の IOCS の処理を d0 = 番号で呼ぶ */
call_orig:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a0
    move.l  %sp@(52),%d0
    jsr     %a0@
    movem.l %sp@+,%d2-%d7/%a2-%a6
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
old_keysns:
    .long   0
old_keyinp:
    .long   0
