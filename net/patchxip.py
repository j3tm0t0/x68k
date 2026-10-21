#!/usr/bin/env python3
"""xip.x (β5, xipb5.lzh) にパッチを当てる。改変済みのバイナリは配布せず、ビルド時に元の xip.x から作る。
usage: patchxip.py IN OUT [--mss N] [--super] [--timer]
  --mss N  : TCP の MSS (既定 1460)。xip は 512 固定で、1 パケットごとに 100～150ms かかるので、
             MSS を大きくするとそのまま速くなる (Pico W netpipe で 4KB/s → 8.5KB/s、2026-10-21)
  --super  : 2 本のスレッド (受信、タイマー) をスーパーバイザモードで作る (_OPEN_PR の SR を $2000 に)。
             効果は確かめていない。既定では当てない
  --timer  : タイマースレッドが表の掃除 (経路表など 5 種) をするのを 0.1 秒に 1 回に間引く。元は 1/100 秒ごとに
             全部なめていて、Z では xip.x の CPU の 2 割近くを使っていた。`tst.l d5; ble` (経過 > 0 なら掃除) を
             `cmp.l d1,d5; blt` にする (d1 には直前の _BITSNS のために 10 が入っている。経過は掃除したときだけ戻る)。
             Pico W netpipe で 17 → 17〜19KB/s。効果が小さいので既定では当てない
書き換える前に、元のバイトとファイル全体の SHA-256 を確かめる (違う版の xip.x には当てない)。"""
import hashlib, sys

SHA256 = "19d3c364fa25f5b499255829f61ab10a717d32136362470def4ab013002ca4ad"
HDR = 0x40                      # X 形式の見出し
MSS_AT = 0x1491A                # 既定の MSS (ワード)。TCP の MSS オプションと送信側の初期値がここを読む
SR_AT = (0x50E, 0x55C)
TIMER_AT = 0x283A               # タイマースレッドの「経過 > 0 なら掃除」 (4A85 6F36)          # スレッドを作る _OPEN_PR に積む SR (move.w #0,-(sp) = 3F3C 0000)

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, dst, mss, sup, tim = args[0], args[1], 1460, False, False
    rest = args[2:]
    while rest:
        a = rest.pop(0)
        if a == "--mss":
            mss = int(rest.pop(0))
        elif a == "--super":
            sup = True
        elif a == "--timer":
            tim = True
        else:
            sys.exit("unknown option " + a)
    d = bytearray(open(src, "rb").read())
    if hashlib.sha256(d).hexdigest() != SHA256:
        sys.exit("patchxip: %s は想定している xip.x β5 ではない (SHA-256 が違う)" % src)
    if not 536 <= mss <= 1460:
        sys.exit("patchxip: MSS は 536～1460")
    o = HDR + MSS_AT
    assert d[o:o + 2] == b"\x02\x00"
    d[o:o + 2] = mss.to_bytes(2, "big")
    if sup:
        for a in SR_AT:
            o = HDR + a
            assert d[o:o + 4] == bytes.fromhex("3f3c0000")
            d[o + 2:o + 4] = b"\x20\x00"
    if tim:
        o = HDR + TIMER_AT
        assert d[o:o + 4] == bytes.fromhex("4a856f36")
        d[o:o + 4] = bytes.fromhex("ba816d36")
    open(dst, "wb").write(d)
    print("patchxip: %s -> %s (MSS %d%s%s)" % (src, dst, mss, ", supervisor threads" if sup else "", ", timer 0.1s" if tim else ""))

main()
