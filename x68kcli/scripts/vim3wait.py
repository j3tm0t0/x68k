#!/usr/bin/env python3
"""vim 3.0 (vim3x687 の vim.x) のキー待ちを、ほかのスレッドに譲るものにする。
usage: vim3wait.py VIM.X

human.c の RealWaitForChar は、待つ時間が無限 (ticks < 0) のとき
    for(; !(i= INPOUT( 0xfe )) ;);
と DOS _INPOUT を回し続け、CPU を使い切る (zos.x のペインでは CPU 87%、ほかのペインが遅くなる)。
時間を決めたときの処理は _CHANGE_PR で譲りながら待つので、無限のときも ticks = 0x7fffffff としてそちらへ回す
(終わりの時刻は ONTIME() + 214748364 で、1 日 = 8640000 より十分先)。
置き換える 14 バイトには絶対番地が無いので、再配置の表は変えなくてよい。"""
import sys

p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
OFF = 0xa40   # ファイルの先頭から (.x のヘッダ 64 バイトを含む)
old = bytes.fromhex('3f3c00feff062600544f67f46036')   # move.w #254,-(sp) / _INPOUT / move.l d0,d3 / addq.w #2,sp / beq 0xa40 / bra 0xa84
new = bytes.fromhex('283c7fffffff60064e714e714e71')   # move.l #$7fffffff,d4 / bra 0xa4e (時間を決めた待ち) / nop x3
cur = bytes(d[OFF:OFF + len(old)])
if cur == new:
    print('vim3wait: already patched'); sys.exit(0)
if cur != old or d[OFF - 2:OFF] != b'\x6c\x0e':   # 直前は bges 0xa4e (ticks >= 0 なら時間を決めた待ちへ)
    sys.exit('vim3wait: %s: 思っていた命令と違う (版が違う?)' % p)
d[OFF:OFF + len(old)] = new
open(p, 'wb').write(d)
print('vim3wait: patched')
