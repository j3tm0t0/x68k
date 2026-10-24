/* zmux.x の入口 (C ライブラリを使わない) */
    .text
    .global _start
_start:
    move.l  %a0,mem_ptr
    move.l  %a2,cmd_line
    jsr     zmux_main
    move.w  %d0,%sp@-
    .short  0xff4c              /* DOS _EXIT2 */

/* long call_entry(void *entry, long cmd): zmux.sys の入口を d0 = 命令で呼ぶ */
    .global call_entry
call_entry:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts

    .bss
    .even
    .global mem_ptr, cmd_line
mem_ptr:  .long 0
cmd_line: .long 0
