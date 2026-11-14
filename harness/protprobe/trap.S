| protprobe: 例外ハンドラと、1 回だけ触ってみる小さな関数
| 例外が起きたら記録して recover_pc へ戻す (68000 のバスエラーは再開できないので、命令はやり直さない)
| 68000 専用: バスエラー / アドレスエラーのフレームは 14 バイト (68010 以降は形が違う)

	.text
	.globl	buserr_h, addrerr_h, priv_h
	.globl	probe_rb, probe_rw, probe_wb, probe_ww
	.globl	probe_movesr, probe_movefromsr, probe_jsr, insn_jsr
	.globl	f_frame, f_usp, f_usptop, f_usp0, f_jtarget
	.globl	insn_rb, insn_rw, insn_wb, insn_ww, insn_movesr
	.globl	f_kind, f_info, f_addr, f_ir, f_sr, f_pc, recover_pc
	.globl	old_bus, old_adr, old_priv

| 例外の PC が試験の関数 (probe_begin..probe_end) の中でなければ、元のハンドラへそのまま渡す
| (OS の裏のタスクなど、ほかのプログラムの例外を横取りしないため)

| グループ 0 のフレーム: +0 情報 (R/W, I/N, FC) +2 アクセス番地 +6 命令 +8 SR +10 PC
| probe_jsr の間は、PC が試験の関数の外 (飛び先) でも、アクセス番地が飛び先なら受ける
buserr_h:
	tst.l	f_jtarget
	beq.s	2f
	move.l	%d0,-(%sp)
	move.l	4+2(%sp),%d0		| アクセス番地
	cmp.l	f_jtarget,%d0
	movem.l	(%sp)+,%d0		| movem はフラグを変えない
	beq.s	jsrgrp0
2:	cmp.l	#probe_begin,10(%sp)
	bcs.s	1f
	cmp.l	#probe_end,10(%sp)
	bcc.s	1f
	move.w	#2,f_kind
	bra.s	grp0
1:	move.l	old_bus,-(%sp)
	rts
addrerr_h:
	cmp.l	#probe_begin,10(%sp)
	bcs.s	1f
	cmp.l	#probe_end,10(%sp)
	bcc.s	1f
	move.w	#3,f_kind
	bra.s	grp0
1:	move.l	old_adr,-(%sp)
	rts
grp0:
	move.w	(%sp),f_info
	move.l	2(%sp),f_addr
	move.w	6(%sp),f_ir
	move.w	8(%sp),f_sr
	move.l	10(%sp),f_pc
	addq.l	#8,%sp			| 普通の 6 バイトのフレームにする
	move.l	recover_pc,2(%sp)
	rte

| probe_jsr のバスエラー: フレーム 14 バイトをそのまま、USP と USP の先頭のロングワードを記録して fault_jsr へ
jsrgrp0:
	move.w	#2,f_kind
	move.l	(%sp),f_frame
	move.l	4(%sp),f_frame+4
	move.l	8(%sp),f_frame+8
	move.w	12(%sp),f_frame+12
	move.l	%a0,-(%sp)
	move.l	%usp,%a0
	move.l	%a0,f_usp
	move.l	(%a0),f_usptop
	move.l	(%sp)+,%a0
	clr.l	f_jtarget
	addq.l	#8,%sp
	move.l	recover_pc,2(%sp)
	rte

| 特権違反: +0 SR +2 PC (PC は違反した命令を指す)
priv_h:
	cmp.l	#probe_begin,2(%sp)
	bcs.s	1f
	cmp.l	#probe_end,2(%sp)
	bcc.s	1f
	move.w	#8,f_kind
	move.w	(%sp),f_sr
	move.l	2(%sp),f_pc
	move.l	recover_pc,2(%sp)
	rte
1:	move.l	old_priv,-(%sp)
	rts

| int probe_xx(addr [, val]): 0 = 触れた、それ以外 = 例外の種類 (2 バスエラー, 3 アドレスエラー, 8 特権違反)
probe_begin:
probe_rb:
	move.l	4(%sp),%a0
	clr.w	f_kind
	move.l	#fault,recover_pc
insn_rb:
	move.b	(%a0),%d0
	moveq	#0,%d0
	rts

probe_rw:
	move.l	4(%sp),%a0
	clr.w	f_kind
	move.l	#fault,recover_pc
insn_rw:
	move.w	(%a0),%d0
	moveq	#0,%d0
	rts

probe_wb:
	move.l	4(%sp),%a0
	move.l	8(%sp),%d1
	clr.w	f_kind
	move.l	#fault,recover_pc
insn_wb:
	move.b	%d1,(%a0)
	moveq	#0,%d0
	rts

probe_ww:
	move.l	4(%sp),%a0
	move.l	8(%sp),%d1
	clr.w	f_kind
	move.l	#fault,recover_pc
insn_ww:
	move.w	%d1,(%a0)
	moveq	#0,%d0
	rts

| move to SR (いまの SR をそのまま書くので、もし通っても状態は変わらない)
probe_movesr:
	clr.w	f_kind
	move.l	#fault,recover_pc
	move.w	%sr,%d1
insn_movesr:
	move.w	%d1,%sr
	moveq	#0,%d0
	rts

| move from SR: 68000 では特権命令ではない (68010 以降は特権違反)
| 戻り値: 下位 16 ビット = SR、例外なら 0x10000 + 種類
probe_movefromsr:
	clr.w	f_kind
	move.l	#fault2,recover_pc
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
fault2:
	moveq	#0,%d0
	move.w	f_kind,%d0
	add.l	#0x10000,%d0
	rts

| int probe_jsr(target): ユーザーモードから jsr (a1) で target へ飛ぶ。target は守られた所 (バスエラーになる所) に限る。
| jsr が戻り番地を積んでからバスエラーになるか、積む前になるかを、記録した USP とフレームで見る (Z-OS の prot_ti_fault)
probe_jsr:
	move.l	4(%sp),%a1
	clr.w	f_kind
	move.l	#fault_jsr,recover_pc
	move.l	%sp,f_usp0
	move.l	%a1,f_jtarget
insn_jsr:
	jsr	(%a1)
	clr.l	f_jtarget		| 戻ってきた (飛び先が守られていなかった)
	moveq	#0,%d0
	rts
fault_jsr:
	move.l	f_usp0,%sp		| jsr が積んだ分を捨てる
	moveq	#0,%d0
	move.w	f_kind,%d0
	rts

fault:
	moveq	#0,%d0
	move.w	f_kind,%d0
	rts
probe_end:

	.bss
	.even
f_kind:	.ds.w	1
f_info:	.ds.w	1
f_addr:	.ds.l	1
f_ir:	.ds.w	1
f_sr:	.ds.w	1
f_pc:	.ds.l	1
recover_pc: .ds.l 1
old_bus: .ds.l	1
old_adr: .ds.l	1
old_priv: .ds.l	1
f_frame: .ds.w	7
f_usp:	.ds.l	1
f_usptop: .ds.l	1
f_usp0:	.ds.l	1
f_jtarget: .ds.l 1
