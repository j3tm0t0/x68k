/*
 * lock.S (速くした名前付きロック) に、元の xip.x の不具合の修正を足したもの (patchxip.py --lockfix)。
 *
 * 不具合: 項目 E (キー K) が空くのを待っている間に、E が放されて別のキー K2 に割り当て直されることがある。
 * 元のコードは、E を取れたらマスターロックを持たずに E のキーを K に書き戻すので、その間に K を探しに来た
 * スレッドは K を見つけられず別の項目に K を作る。同じキーが 2 つでき、2 本のスレッドが同時に K を持つ
 * (mutex.py で、割り込みでどこでも切り替わる条件にすると元のコードでも起きる)。
 * 修正: E を取れたら、マスターロックを取り直してキーがまだ K かを確かめる。違っていたら E を放して探し直す。
 * もう 1 つ: 空いた項目を割り当てるとき、元は「フラグが 0 か調べてから 1 を書く」ので、その間に待っている側が
 * tas で同じ項目を取ると両方が持つ。割り当ても tas で取る (フラグは 1 ではなく 0x80 になる。使用中かどうかは
 * 長語が 0 かどうかでしか見ていない)。
 */
    .set    TABLE, 0x1547a
    .set    IRQFLAG, 0x19ab4
    .set    MSG, 0xf1c
    .set    ACQUIRE, 0x1034
    .set    RELEASE, 0x1044
    .set    BASE, 0xf4c

    .text
    .short  0
/* void lock(long key) */
lock:
    tst.l   4(%sp)
    beq.s   9f
R_irq = . + 2
    tst.l   IRQFLAG
    bne.w   irq
    bsr.w   ACQUIRE
    move.l  4(%sp),%d1                  /* キー (_CHANGE_PR のあとで読む) */
scan:                                   /* マスターロックを持って、d1 = キー */
    bsr.s   find
    bne.s   2f
    /* 見つからない: 空いている項目を tas で取る。元は「フラグが 0 か調べてから 1 を書く」で、その間に
       マスターロックを持たずに待っている側が tas で同じ項目を取ると、両方が持っていることになった */
1:  movea.l tblp(%pc),%a0
    addq.w  #4,%a0
    moveq   #31,%d0
3:  tst.l   (%a0)
    bne.s   4f
    tas     3(%a0)
    beq.s   5f
4:  addq.w  #8,%a0
    dbf     %d0,3b
    bra.s   1b
5:  clr.l   -4(%a0)                     /* 元と同じく、キーは消してから書く */
    move.l  %d1,-4(%a0)
    bra.w   RELEASE
2:  bsr.w   RELEASE
    movem.l %d3/%a4,-(%sp)
    move.l  %d1,%d3
    lea     7(%a0),%a4
7:  tas     (%a4)
    beq.s   6f
    .short  0xffff                      /* DOS _CHANGE_PR */
    bra.s   7b
6:  bsr.w   ACQUIRE                     /* 取れた。キーがまだ自分のものかを、マスターロックを持って確かめる */
    move.l  %d3,%d1
    lea     -7(%a4),%a0
    movem.l (%sp)+,%d3/%a4
    cmp.l   (%a0),%d1
    beq.w   RELEASE                     /* 自分のもの: d0 = 0 にして戻る */
    clr.l   4(%a0)                      /* 別のキーに割り当て直されていた: 放して探し直す */
    bra.s   scan
9:  rts

R_tblp = .
tblp:  .long  TABLE
R_msgp = .
msgp:  .long  MSG

find:
    movea.l tblp(%pc),%a0
    moveq   #31,%d0
1:  cmp.l   (%a0)+,%d1
    beq.s   2f
    addq.w  #4,%a0
    dbf     %d0,1b
    cmp.w   %d0,%d0
    rts
2:  subq.w  #4,%a0
    move.l  %a0,%d0
    rts

    .org    0xfe8 - BASE
/* long lookup(long key) */
lookup:
    move.l  4(%sp),%d1
lookup1:                                /* d1 = キー、条件コードは d1 のもの */
    beq.s   1f
    bsr.s   find
    beq.s   1f
    move.l  4(%a0),%d0
    rts
1:  moveq   #0,%d0
    rts
/* 割り込み中のロック: 使用中なら panic を表示するだけ */
irq:
    move.l  4(%sp),%d1
    bsr.s   lookup1
    beq.s   1f
    movea.l msgp(%pc),%a1
    moveq   #0x21,%d0
    trap    #15
1:  rts

    .org    0x100e - BASE
/* void unlock(long key) (d0 は元と同じく項目の番号、無ければ 32) */
unlock:
    move.l  4(%sp),%d1
    beq.s   9f
    bsr.s   find
    beq.s   8f
    moveq   #0,%d2
    move.l  %d2,4(%a0)
    move.l  %a0,%d0
    sub.l   tblp(%pc),%d0
    lsr.l   #3,%d0
9:  rts
8:  moveq   #32,%d0
    moveq   #31,%d2
    rts
    .org    0x1034 - BASE
