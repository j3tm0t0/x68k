/* reboot.x: IPL ROM のリセット処理へ飛んで再起動する */
    .text
    .global _start
_start:
    clr.l   %sp@-
    .short  0xff20              /* DOS _SUPER */
    ori.w   #0x0700,%sr
    movea.l 0x00ff0000,%sp      /* リセット時の SSP と PC */
    movea.l 0x00ff0004,%a0
    jmp     %a0@
