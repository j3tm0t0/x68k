/* x68str.S: 68000 向けの memcpy / memmove / memset / memcmp / memchr / strlen / strcmp / strchr。
 *
 * elf2x68k の newlib はこれらを大きさ優先の C (1 バイトごとに添字付きの番地で読み書き) で持っていて、
 * 1 バイト 40〜80 サイクルかかる。ここでは 2 つの番地の偶奇がそろっていれば 4 バイトずつ、
 * そろわなければ (a0)+ と dbra の 1 バイトずつで処理する。68000 は奇数番地への語・長語のアクセスで止まるので、
 * 4 バイトずつにする前に 1 バイト進めて偶数番地にそろえる。
 * 引数はスタック (4(sp) から)、戻り値は d0。d0-d1/a0-a1 だけを壊し、ほかは使うなら保存する。
 * x68shim と一緒に GNU の道具へリンクされ、libc.a の同じ名前の関数より先に使われる。関数ごとに節を分け、
 * --gc-sections で使う分だけ残す (memmove は memcpy の前から写す処理を使うので同じ節)。試験は tests/str/run.sh */

/* 32 ビットの回数で dbcc を回すときは、下位 16 ビットを使い切ったら swap して上位を 1 減らし、続ける */

    .section .text.memcpy,"ax",@progbits
/* void *memcpy(void *dst, const void *src, size_t n) */
    .globl  memcpy
    .even
memcpy:
    movem.l 4(%sp),%a0-%a1          /* a0 = dst, a1 = src */
    move.l  12(%sp),%d1             /* d1 = n */
.Lfwd:
    moveq   #16,%d0
    cmp.l   %d0,%d1
    bcs.s   .Lfbyte                 /* 16 バイト未満は 1 バイトずつ */
    move.w  %a0,%d0
    sub.w   %a1,%d0
    btst    #0,%d0
    bne.s   .Lfbyte                 /* 偶奇がそろわない */
    move.w  %a0,%d0
    btst    #0,%d0
    beq.s   1f
    move.b  (%a1)+,(%a0)+           /* 偶数番地にそろえる */
    subq.l  #1,%d1
1:  move.l  %d1,%d0
    lsr.l   #5,%d0                  /* 32 バイトの塊の数 (そろえた後は 0 のこともある) */
    beq.s   6f
2:  move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    subq.l  #1,%d0
    bne.s   2b
6:  btst    #4,%d1
    beq.s   7f
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
7:  btst    #3,%d1
    beq.s   3f
    move.l  (%a1)+,(%a0)+
    move.l  (%a1)+,(%a0)+
3:  btst    #2,%d1
    beq.s   4f
    move.l  (%a1)+,(%a0)+
4:  btst    #1,%d1
    beq.s   5f
    move.w  (%a1)+,(%a0)+
5:  btst    #0,%d1
    beq     .Lcpyret
    move.b  (%a1)+,(%a0)+
    bra     .Lcpyret
.Lfbyte:
    move.l  %d1,%d0
    beq     .Lcpyret
    subq.l  #1,%d0
1:  move.b  (%a1)+,(%a0)+
    dbra    %d0,1b
    swap    %d0
    dbra    %d0,2f
    bra     .Lcpyret
2:  swap    %d0
    bra.s   1b
.Lcpyret:
    move.l  4(%sp),%d0
    rts

/* void *memmove(void *dst, const void *src, size_t n): 重なっていて dst が後ろなら末尾から写す */
    .globl  memmove
    .even
memmove:
    movem.l 4(%sp),%a0-%a1
    move.l  12(%sp),%d1
    cmpa.l  %a1,%a0
    bls     .Lfwd                   /* dst <= src: 前から */
    move.l  %a1,%d0
    add.l   %d1,%d0
    cmpa.l  %d0,%a0
    bcc     .Lfwd                   /* dst >= src + n: 重ならない */
    adda.l  %d1,%a0                 /* 末尾から */
    adda.l  %d1,%a1
    moveq   #16,%d0
    cmp.l   %d0,%d1
    bcs.s   .Lbbyte
    move.w  %a0,%d0
    sub.w   %a1,%d0
    btst    #0,%d0
    bne.s   .Lbbyte
    move.w  %a0,%d0
    btst    #0,%d0
    beq.s   1f
    move.b  -(%a1),-(%a0)
    subq.l  #1,%d1
1:  move.l  %d1,%d0
    lsr.l   #5,%d0
    beq.s   6f
2:  move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    subq.l  #1,%d0
    bne.s   2b
6:  btst    #4,%d1
    beq.s   7f
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
7:  btst    #3,%d1
    beq.s   3f
    move.l  -(%a1),-(%a0)
    move.l  -(%a1),-(%a0)
3:  btst    #2,%d1
    beq.s   4f
    move.l  -(%a1),-(%a0)
4:  btst    #1,%d1
    beq.s   5f
    move.w  -(%a1),-(%a0)
5:  btst    #0,%d1
    beq     .Lcpyret
    move.b  -(%a1),-(%a0)
    bra     .Lcpyret
.Lbbyte:
    move.l  %d1,%d0
    beq     .Lcpyret
    subq.l  #1,%d0
1:  move.b  -(%a1),-(%a0)
    dbra    %d0,1b
    swap    %d0
    dbra    %d0,2f
    bra     .Lcpyret
2:  swap    %d0
    bra.s   1b

    .section .text.memset,"ax",@progbits
/* void *memset(void *s, int c, size_t n) */
    .globl  memset
    .even
memset:
    move.l  4(%sp),%a0
    moveq   #0,%d1
    move.b  11(%sp),%d1             /* c の下位 8 ビット */
    move.l  12(%sp),%d0             /* n */
    cmpi.l  #16,%d0
    bcs.s   .Lsbyte
    move.l  %d1,%a1                 /* d1 = cccccccc */
    lsl.w   #8,%d1
    add.w   %a1,%d1
    move.w  %d1,%a1
    swap    %d1
    move.w  %a1,%d1
    move.w  %a0,%a1
    exg     %d0,%a1                 /* d0 = 番地の下位、a1 = n (exg は条件コードを変えない) */
    btst    #0,%d0
    exg     %d0,%a1
    beq.s   1f
    move.b  %d1,(%a0)+
    subq.l  #1,%d0
1:  move.l  %d0,%a1                 /* a1 = 残り */
    lsr.l   #4,%d0
    beq.s   6f
2:  move.l  %d1,(%a0)+
    move.l  %d1,(%a0)+
    move.l  %d1,(%a0)+
    move.l  %d1,(%a0)+
    subq.l  #1,%d0
    bne.s   2b
6:  move.l  %a1,%d0
    btst    #3,%d0
    beq.s   3f
    move.l  %d1,(%a0)+
    move.l  %d1,(%a0)+
3:  btst    #2,%d0
    beq.s   4f
    move.l  %d1,(%a0)+
4:  btst    #1,%d0
    beq.s   5f
    move.w  %d1,(%a0)+
5:  btst    #0,%d0
    beq.s   .Lsret
    move.b  %d1,(%a0)+
    bra.s   .Lsret
.Lsbyte:
    tst.l   %d0
    beq.s   .Lsret
    subq.l  #1,%d0
1:  move.b  %d1,(%a0)+
    dbra    %d0,1b
    swap    %d0
    dbra    %d0,2f
    bra.s   .Lsret
2:  swap    %d0
    bra.s   1b
.Lsret:
    move.l  4(%sp),%d0
    rts

    .section .text.memcmp,"ax",@progbits
/* int memcmp(const void *a, const void *b, size_t n): 違う最初のバイトを unsigned char として引いた値 */
    .globl  memcmp
    .even
memcmp:
    movem.l 4(%sp),%a0-%a1
    move.l  12(%sp),%d1
    moveq   #16,%d0
    cmp.l   %d0,%d1
    bcs.s   .Lcbyte
    move.w  %a0,%d0
    sub.w   %a1,%d0
    btst    #0,%d0
    bne.s   .Lcbyte
    move.w  %a0,%d0
    btst    #0,%d0
    beq.s   1f
    cmpm.b  (%a1)+,(%a0)+
    bne.s   .Lcdiff
    subq.l  #1,%d1
1:  move.l  %d1,%d0
    lsr.l   #2,%d0                  /* 4 バイトの数 (4 以上) */
2:  cmpm.l  (%a1)+,(%a0)+
    bne.s   3f
    subq.l  #1,%d0
    bne.s   2b
    andi.l  #3,%d1                  /* 残り 0〜3 バイト */
    bra.s   .Lcbyte
3:  subq.l  #4,%a0                  /* 違う 4 バイトを 1 バイトずつ見直す */
    subq.l  #4,%a1
    moveq   #4,%d1
.Lcbyte:
    move.l  %d1,%d0
    beq.s   .Lceq
    subq.l  #1,%d0
1:  cmpm.b  (%a1)+,(%a0)+
    dbne    %d0,1b
    bne.s   .Lcdiff
    swap    %d0
    dbra    %d0,2f
.Lceq:
    moveq   #0,%d0
    rts
2:  swap    %d0
    bra.s   1b
.Lcdiff:
    moveq   #0,%d0
    moveq   #0,%d1
    move.b  -(%a0),%d0
    move.b  -(%a1),%d1
    sub.l   %d1,%d0
    rts

    .section .text.memchr,"ax",@progbits
/* void *memchr(const void *s, int c, size_t n) */
    .globl  memchr
    .even
memchr:
    move.l  4(%sp),%a0
    move.b  11(%sp),%d1
    move.l  12(%sp),%d0
    beq.s   3f
    subq.l  #1,%d0
1:  cmp.b   (%a0)+,%d1
    dbeq    %d0,1b
    beq.s   4f
    swap    %d0
    dbra    %d0,2f
3:  moveq   #0,%d0
    rts
2:  swap    %d0
    bra.s   1b
4:  move.l  %a0,%d0
    subq.l  #1,%d0
    rts

    .section .text.strlen,"ax",@progbits
/* size_t strlen(const char *s) */
    .globl  strlen
    .even
strlen:
    move.l  4(%sp),%a0
    move.l  %a0,%d0
1:  tst.b   (%a0)+
    beq.s   2f
    tst.b   (%a0)+
    bne.s   1b
2:  move.l  %a0,%d1
    sub.l   %d0,%d1
    subq.l  #1,%d1
    move.l  %d1,%d0
    rts

    .section .text.strcmp,"ax",@progbits
/* int strcmp(const char *a, const char *b) */
    .globl  strcmp
    .even
strcmp:
    movem.l 4(%sp),%a0-%a1
1:  move.b  (%a0)+,%d0
    beq.s   2f
    cmp.b   (%a1)+,%d0
    beq.s   1b
    moveq   #0,%d1
    move.b  -(%a1),%d1
    andi.l  #0xff,%d0
    sub.l   %d1,%d0
    rts
2:  moveq   #0,%d0                  /* a が終わった: 0 - b の文字 */
    move.b  (%a1),%d0
    neg.l   %d0
    rts

    .section .text.strchr,"ax",@progbits
/* char *strchr(const char *s, int c): c が 0 なら終わりの 0 の番地 */
    .globl  strchr
    .even
strchr:
    move.l  4(%sp),%a0
    move.b  11(%sp),%d1
1:  move.b  (%a0)+,%d0
    cmp.b   %d0,%d1
    beq.s   2f
    tst.b   %d0
    bne.s   1b
    moveq   #0,%d0
    rts
2:  move.l  %a0,%d0
    subq.l  #1,%d0
    rts
