	.text
	.globl _start
_start:
	move.l #0xbf0000,%sp
	jsr bench
1:	bra 1b
