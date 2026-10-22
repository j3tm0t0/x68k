/* tis.x の text の先頭。libsocket / libnetwork は常駐ブロックのプログラム領域が "TCP/IP" で始まるものを探し、
 * text + 8 を d0 = 命令番号、a0 = 引数で jsr する (d0 が戻り値、d0-d2/a0-a2 は壊してよい)。
 * text + 12 の "TISTACK1" は tis.x 自身が常駐しているものを見分ける印 (xip.x と区別する) */
    .text
    .ascii  "TCP/IP\0\0"
    .global ti_entry
ti_entry:
    bra.w   ti_entry1
    .ascii  "TISTACK1"
ti_entry1:
    move.l  %a0,%sp@-
    move.l  %d0,%sp@-
    jsr     ti_dispatch
    addq.l  #8,%sp
    rts

/* long call_ti(void *entry, long cmd, void *arg): 常駐している tis.x の入口を呼ぶ (tis.x -s / -r) */
    .global call_ti
call_ti:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

/* 起動: Human68k は a0 = メモリ管理ポインタ, a1 = プログラムの終わり, a2 = コマンド行を渡す。
 * 受信リングは BSS の後ろに置くので、スタックは BSS の中の自前のものに移す */
    .global _start
_start:
    move.l  %a0,mem_ptr
    move.l  %a1,prg_end
    move.l  %a2,cmd_line
    lea     stack_top,%sp
    jsr     tis_main
    move.w  %d0,%sp@-
    .short  0xff4c              /* DOS _EXIT2 */

    .bss
    .even
    .global mem_ptr, prg_end, cmd_line
mem_ptr:    .long 0
prg_end:    .long 0
cmd_line:   .long 0
    .space  4096
stack_top:
