/* start.S -- rx.x の入口。newlib の crt0 を使わず、プロセスのメモリを必要な分だけに縮めてから main を呼ぶ。
 * Human68k は a0 = メモリ管理ポインタ、a1 = プログラムの末尾 (bss の後)、a2 = コマンドライン を渡す。
 * スタックは bss の中に持つ (_SETBLOCK で縮めた後も使える)。子プロセス (COMMAND.X) は残りのメモリで動く。 */
    .text
    .global _start
_start:
    lea     stack_top,%sp
    move.l  %a2,cmdline
    lea     16(%a0),%a0             /* メモリブロックの先頭 (PSP) */
    suba.l  %a0,%a1                 /* 使う長さ */
    move.l  %a1,-(%sp)
    move.l  %a0,-(%sp)
    .short  0xff4a                  /* DOS _SETBLOCK */
    addq.l  #8,%sp
    move.l  cmdline,-(%sp)
    jsr     rx_main
    move.w  %d0,-(%sp)
    .short  0xff4c                  /* DOS _EXIT2 */

    .bss
    .even
    .global cmdline
cmdline:
    .skip   4
    .skip   6144
stack_top:
