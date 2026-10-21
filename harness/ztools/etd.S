    .text
    .global call_etd
call_etd:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts
