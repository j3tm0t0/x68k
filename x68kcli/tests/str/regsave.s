/* int regcheck(void *fn, long x, long y, long z): d2-d7 / a2-a6 を決まった値にして fn(x, y, z) を呼び、
   戻ったあとにどれかが変わっていれば 1 を返す (呼ばれた側が保存すべきレジスタを壊していないかの試験) */
	.text
	.globl regcheck
regcheck:
	movem.l %d2-%d7/%a2-%a6,-(%sp)
	move.l 48(%sp),%a0
	move.l 60(%sp),-(%sp)
	move.l 60(%sp),-(%sp)
	move.l 60(%sp),-(%sp)
	move.l #0x22222222,%d2
	move.l #0x33333333,%d3
	move.l #0x44444444,%d4
	move.l #0x55555555,%d5
	move.l #0x66666666,%d6
	move.l #0x77777777,%d7
	move.l #0xa2a2a2a2,%a2
	move.l #0xa3a3a3a3,%a3
	move.l #0xa4a4a4a4,%a4
	move.l #0xa5a5a5a5,%a5
	move.l #0xa6a6a6a6,%a6
	jsr (%a0)
	lea 12(%sp),%sp
	moveq #1,%d0
	cmp.l #0x22222222,%d2
	bne.s 9f
	cmp.l #0x33333333,%d3
	bne.s 9f
	cmp.l #0x44444444,%d4
	bne.s 9f
	cmp.l #0x55555555,%d5
	bne.s 9f
	cmp.l #0x66666666,%d6
	bne.s 9f
	cmp.l #0x77777777,%d7
	bne.s 9f
	cmpa.l #0xa2a2a2a2,%a2
	bne.s 9f
	cmpa.l #0xa3a3a3a3,%a3
	bne.s 9f
	cmpa.l #0xa4a4a4a4,%a4
	bne.s 9f
	cmpa.l #0xa5a5a5a5,%a5
	bne.s 9f
	cmpa.l #0xa6a6a6a6,%a6
	bne.s 9f
	moveq #0,%d0
9:	movem.l (%sp)+,%d2-%d7/%a2-%a6
	rts
