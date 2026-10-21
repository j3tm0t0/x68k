/*
 * xip.x (β5) の名前付きロック (0xF4E-0x1033) の書き直し。表 (0x1547A、8 バイト × 32: キー, 使用中フラグ) を
 * 先頭から探すのは元と同じで、探すループだけを速くした (1 項目 68 → 40 サイクル)。
 * 「前回見つけた項目から先に確かめる」は使えない: 元のコードでは、待っている間に項目が別のキーに割り当て直されると、
 * 同じキーが表に 2 つできる (差分テストで見つかった。元の不具合だが、動作を変えないため直さない)。
 * そのとき元は先頭に近い方を使うので、それと同じ順に探す必要がある。
 * 入口の番地 (0xF4E ロック, 0xFE8 参照, 0x100E アンロック) と、0x1034 / 0x1044 (マスターロック) は変えない。
 * 番地は再配置前 (xip.x の先頭 = 0)。R_ で始まるラベルの位置の長語が再配置される (patchxip.py が再配置表を作り直す)
 */
    .set    TABLE, 0x1547a
    .set    IRQFLAG, 0x19ab4            /* 割り込み中 (ロックせず、使用中なら panic を出すだけ) */
    .set    MSG, 0xf1c                  /* "panic: semaphore block occured while interrupt." */
    .set    ACQUIRE, 0x1034             /* マスターロックを取る (a0 を壊す。取れるまで _CHANGE_PR) */
    .set    RELEASE, 0x1044             /* マスターロックを放す (d0 = 0 になる) */
    .set    BASE, 0xf4c                 /* .text は 4 バイト境界になるので、0xF4C からリンクして先頭の 2 バイトを捨てる */

    .text
    .short  0
/* void lock(long key) */
lock:
    tst.l   4(%sp)
    beq.s   9f
R_irq = . + 2
    tst.l   IRQFLAG
    bne.s   8f
    bsr.w   ACQUIRE
    move.l  4(%sp),%d1                  /* キー (_CHANGE_PR のあとで読む) */
    bsr.s   find
    bne.s   2f
    /* 見つからない: 使用中フラグが 0 の最初の項目を取る (表が埋まっていれば空くまで探し続ける。元と同じ) */
1:  movea.l tblp(%pc),%a0
    addq.w  #4,%a0
    moveq   #31,%d0
3:  tst.l   (%a0)
    beq.s   4f
    addq.w  #8,%a0
    dbf     %d0,3b
    bra.s   1b
4:  clr.l   -4(%a0)
    moveq   #1,%d0
    move.l  %d0,(%a0)
    move.l  %d1,-(%a0)
    bra.w   RELEASE                     /* d0 = 0 にして戻る */
    /* 見つかった: マスターロックを放してから、その項目が空くのを待つ */
2:  bsr.w   RELEASE
    movem.l %d3/%a4,-(%sp)
    move.l  %d1,%d3
    lea     7(%a0),%a4
5:  tas     (%a4)
    beq.s   6f
    .short  0xffff                      /* DOS _CHANGE_PR */
    bra.s   5b
6:  move.l  %d3,-7(%a4)
    movem.l (%sp)+,%d3/%a4
9:  rts
    /* 割り込み中: 使用中なら panic を表示するだけ */
8:  move.l  4(%sp),-(%sp)
    bsr.s   lookup
    addq.l  #4,%sp                      /* addq は条件コードを変えない (lookup の move / moveq の Z のまま) */
    beq.s   9b
    movea.l msgp(%pc),%a1
    moveq   #0x21,%d0                   /* IOCS _B_PRINT */
    trap    #15
    rts

/* d1 = キー (0 以外) の項目を先頭から探す。見つかれば Z = 0 で a0 = 項目、無ければ Z = 1。d0, a0 を壊す
   (元のロックは d2 に触らないので、ここでも使わない。差分テストで見つかった) */
find:
    movea.l tblp(%pc),%a0
    moveq   #31,%d0
1:  cmp.l   (%a0)+,%d1
    beq.s   2f
    addq.w  #4,%a0
    dbf     %d0,1b
    cmp.w   %d0,%d0                     /* Z = 1 */
    rts
2:  subq.w  #4,%a0
    move.l  %a0,%d0                     /* Z = 0 (a0 は 0 でない) */
    rts

    .org    0xfe8 - BASE                /* ここまでに収まらなければ as がエラーにする */
/* long lookup(long key): 使用中フラグ (無ければ 0) */
lookup:
    bra.w   lookup_body
lookup_body:
    move.l  4(%sp),%d1
    beq.s   1f
    bsr.s   find
    beq.s   1f
    move.l  4(%a0),%d0
    rts
1:  moveq   #0,%d0
    rts
R_tblp = .
tblp:  .long  TABLE
R_msgp = .
msgp:  .long  MSG

    .org    0x100e - BASE
/* void unlock(long key) (d0 は元と同じく項目の番号、無ければ 32) */
unlock:
    bra.w   unlock_body
unlock_body:
    move.l  4(%sp),%d1
    beq.s   9f
    bsr.s   find
    beq.s   8f
    moveq   #0,%d2                      /* 元と同じく d2 = 0 */
    move.l  %d2,4(%a0)
    move.l  %a0,%d0
    sub.l   tblp(%pc),%d0
    lsr.l   #3,%d0
9:  rts
8:  moveq   #32,%d0
    moveq   #31,%d2                     /* 元と同じく d0 = 32, d2 = 31 */
    rts
    .org    0x1034 - BASE
