/* slipd.x の入口 (C ライブラリを使わない)。Human68k は a0 = メモリ管理ポインタ, a1 = プログラムの終わりを渡す */
    .text
    .global _start
_start:
    move.l  %a0,mem_ptr
    move.l  %a1,prg_end
    move.l  %a2,cmd_line
    jsr     slipd_main
    move.w  %d0,%sp@-
    .short  0xff4c              /* DOS _EXIT2 */

/* long call_etd(void *entry, long cmd, void *arg): slip.sys の ETD 入口を d0 = 命令, a0 = 引数で呼ぶ */
    .global call_etd
call_etd:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

    .bss
    .even
    .global mem_ptr, prg_end, cmd_line
mem_ptr:  .long 0
prg_end:  .long 0
cmd_line: .long 0
