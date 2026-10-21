/*
 * npd.sys -- ESP/X (xip.x / inetd.x) 用 netpipe ネットワークドライバ
 * デバイスヘッダと、アセンブラで書く必要のある入口 (ETD 命令 / ハンドラ呼び出し)
 *
 * xip.x はデバイスチェインから名前 "/dev/ep0" の直後に "EthD" があるものを探し、
 * ヘッダ先頭 + 0x1e の命令入口を SUPER_JSR で呼ぶ (d0 = 命令番号, a0 = 引数)。
 * ESP/X PACK-B の libether.c と、IIJ-PPP Human68k 版の ppp.sys と同じ配置にしている。
 */
    .text
    .global devheader
devheader:
    .long   -1                  /* +0x00 次のデバイス */
    .word   0x8004              /* +0x04 属性 (ppp.sys と同じ) */
    .long   strategy            /* +0x06 ストラテジルーチン */
    .long   interrupt_asm       /* +0x0a 割り込みルーチン */
    .ascii  "/dev/ep0"          /* +0x0e デバイス名 */
    .ascii  "EthD"              /* +0x16 ESP/X ドライバの印 */
    .ascii  "NPD "              /* +0x1a ドライバ識別子 */
etd_entry:                      /* +0x1e ETD 命令入口: d0 = 命令, a0 = 引数 → d0 = 結果 */
    movem.l %d1-%d7/%a0-%a6,%sp@-
    move.l  %a0,%sp@-
    move.l  %d0,%sp@-
    jsr     etd_dispatch
    addq.l  #8,%sp
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
    move.b  %d0,%a5@(3)         /* errl */
    lsr.w   #8,%d0
    move.b  %d0,%a5@(4)         /* errh */
    movem.l %sp@+,%d0-%d7/%a0-%a6
    rts

/* void call_handler(void *func, long size, void *pkt, long id)
 * xip.x の受信ハンドラを C の呼び出し規約 (スタック渡し) で呼ぶ。
 * ハンドラは d0-d2/a0-a2 を壊してよい約束なので、gcc が保存を期待する d2/a2 以降をここで守る */
    .global call_handler
call_handler:
    movem.l %d2-%d7/%a2-%a6,%sp@-      /* 11 本 = 44 バイト。引数は sp+48 から */
    move.l  %sp@(60),%sp@-             /* id */
    move.l  %sp@(60),%sp@-             /* pkt */
    move.l  %sp@(60),%sp@-             /* size */
    movea.l %sp@(60),%a0               /* func */
    jsr     %a0@
    lea     %sp@(12),%sp
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

/* DOS _PRINT (初期化時のタイトル表示) */
    .global dos_print
dos_print:
    move.l  %sp@(4),%sp@-
    .short  0xff09
    addq.l  #4,%sp
    rts

    .data           /* .bss は初期化で 0 クリアするので、初期化中に使う reqheader は .data に置く */
    .even
reqheader:
    .long   0
