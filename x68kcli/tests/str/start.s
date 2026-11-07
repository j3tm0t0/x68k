	.text
	.globl _start
_start:
	move.l #0xbf0000,%sp
	move.l #1,0x7fffe0
	jsr bench_main
	move.l #2,0x7fffe0
1:	bra 1b
