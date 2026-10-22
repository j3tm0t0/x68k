/* long accel_call_etd(void *entry, long cmd, void *arg): npd.sys の ETD 入口を d0 = 命令, a0 = 引数で呼ぶ
 * (harness/ztools/etd.S と同じ。スーパーバイザモードで呼ぶこと) */
    .text
    .global accel_call_etd
accel_call_etd:
    movem.l %d2-%d7/%a2-%a6,%sp@-
    movea.l %sp@(48),%a1
    move.l  %sp@(52),%d0
    movea.l %sp@(56),%a0
    jsr     %a1@
    movem.l %sp@+,%d2-%d7/%a2-%a6
    rts
