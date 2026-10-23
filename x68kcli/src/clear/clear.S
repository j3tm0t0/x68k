/* clear.x: 画面を消してカーソルを左上に (ESC [ 2 J。Human68k の CON は消して左上へ、Z-OS のペインはそのペインだけ) */
    .text
    .global _start
_start:
    pea     msg(%pc)
    .short  0xff09              /* DOS _PRINT */
    addq.l  #4,%sp
    .short  0xff00              /* DOS _EXIT */
msg:
    .byte   0x1b
    .ascii  "[2J"
    .byte   0
