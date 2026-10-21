#!/usr/bin/env python3
"""xip.x (β5, xipb5.lzh) にパッチを当てる。改変済みのバイナリは配布せず、ビルド時に元の xip.x から作る。
usage: patchxip.py IN OUT [--mss N] [--super] [--timer] [--lock | --lockfix] [--yield]
  --mss N  : TCP の MSS (既定 1460)。xip は 512 固定で、1 パケットごとに 100～150ms かかるので、
             MSS を大きくするとそのまま速くなる (Pico W netpipe で 4KB/s → 8.5KB/s、2026-10-21)
  --super  : 2 本のスレッド (受信、タイマー) をスーパーバイザモードで作る (_OPEN_PR の SR を $2000 に)。
             効果は確かめていない。既定では当てない
  --timer  : タイマースレッドが表の掃除 (経路表など 5 種) をするのを 0.1 秒に 1 回に間引く。元は 1/100 秒ごとに
             全部なめていて、Z では xip.x の CPU の 2 割近くを使っていた。`tst.l d5; ble` (経過 > 0 なら掃除) を
             `cmp.l d1,d5; blt` にする (d1 には直前の _BITSNS のために 10 が入っている。経過は掃除したときだけ戻る)。
             Pico W netpipe で 17 → 17〜19KB/s、npd では 36 → 40〜45KB/s (x68kcli は XIPTIMER=1 で既定で当てる)
  --lock   : 名前付きロック (0xF4E-0x1033) を、表を探すループの速い版に置き換える (harness/xiptest/lock.S)。
             元と同じ順に探すので動作は同じ (Mac の上の 68000 で元と比べる差分テスト harness/xiptest/difflock.py
             で確かめた)。命令数はキーが 4 個以上のとき約 1.5 分の 1。再配置表も作り直す
  --lockfix: --lock に、元の不具合の修正を足したもの (harness/xiptest/lockfix.S)。待っている間に項目が別の
             キーに割り当て直されると、同じキーが表に 2 つでき、2 本のスレッドが同時に同じキーを持てた。
             取れたあとでマスターロックを持ってキーを確かめ直す (mutex.py で、割り込みでどこでも切り替わる条件で確かめる)
  --lockfix: --lock に、元の不具合の修正を足したもの (harness/xiptest/lockfix.S)。待っている間に項目が別の
             キーに割り当て直されると、同じキーが表に 2 つでき、2 本のスレッドが同時に同じキーを持てた。
             取れたあとでマスターロックを持ってキーを確かめ直す (mutex.py で、割り込みでどこでも切り替わる条件で確かめる)
  --yield  : 受信スレッドとタイマースレッドは、処理 (0x26B0 / 0x2792) の最後と、スレッドの繰り返し (0x2FA / 0x32E) の
             両方で _CHANGE_PR を呼び、1 周に 2 回譲っていた。処理の中の 2 か所 (0x2782 の bsr.w、0x2874 の bsr.s) を nop にする
             (2 つの処理はスレッドの繰り返しからしか呼ばれない)
書き換える前に、元のバイトとファイル全体の SHA-256 を確かめる (違う版の xip.x には当てない)。"""
import hashlib, struct, sys


def reloc_parse(tab):
    """X 形式の再配置表: 語は前の位置からの差 (1 なら続く長語が差)。その位置の長語に先頭番地を足す"""
    offs, o, i = [], 0, 0
    while i < len(tab):
        w = struct.unpack(">H", tab[i:i + 2])[0]
        i += 2
        if w == 1:
            w = struct.unpack(">I", tab[i:i + 4])[0]
            i += 4
        o += w
        offs.append(o)
    return offs


def reloc_build(offs):
    out, prev = b"", 0
    for o in offs:
        d = o - prev
        out += struct.pack(">H", d) if d < 0x10000 else struct.pack(">HI", 1, d)
        prev = o
    return out

SHA256 = "19d3c364fa25f5b499255829f61ab10a717d32136362470def4ab013002ca4ad"
HDR = 0x40                      # X 形式の見出し
MSS_AT = 0x1491A                # 既定の MSS (ワード)。TCP の MSS オプションと送信側の初期値がここを読む
SR_AT = (0x50E, 0x55C)
LOCK_AT = (0xF4E, 0x1034)       # 名前付きロック (ロック / 参照 / アンロック)
LOCK_OLD_SHA256 = "7966d31083e7412eeb34bfc73b92781030d06a39a95997502feb524de0215d95"
LOCK_NEW = bytes.fromhex("4aaf000467544ab900019ab4664e610000d6222f000461586622207a00965848701f4a906708504851c8fff860ec42a8fffc700120802101600000bc610000b848e71008260149e800074ad46704ffff60f82943fff94cdf10084e752f2f00046138588f67f4227a004e70214e4f4e75207a0040701fb298670a584851c8fff8b0404e75594820084e750000000000000000000000000000000060000002222f0004670a61ca6706202800044e7570004e750001547a00000f1c00000000000060000002222f0004671261a46710740021420004200890baffdae6884e757020741f4e750000")
LOCK_REL = (0xF56, 0x1004, 0x1000)                  # 新しいコードの中で再配置する長語の位置
LOCKFIX_NEW = bytes.fromhex("4aaf0004676c4ab900019ab4660000a0610000d4222f000461626626207a00565848701f4a9066064ae800036708504851c8fff260e642a8fffc2141fffc600000b6610000b248e71008260149e800074ad46704ffff60f86100008c220341ecfff94cdf1008b2906700008c42a8000460a64e750001547a00000f1c207afff6701fb298670a584851c8fff8b0404e75594820084e7500000000222f0004670a61da6706202800044e7570004e75222f000461ea6708227affc070214e4f4e75222f0004671261b46710740021420004200890baffa0e6884e757020741f4e75000000000000")
LOCKFIX_REL = (0xF56, 0xFC6, 0xFC2)
LOCKFIX_NEW = bytes.fromhex("4aaf0004676c4ab900019ab4660000a0610000d4222f000461626626207a00565848701f4a9066064ae800036708504851c8fff260e642a8fffc2141fffc600000b6610000b248e71008260149e800074ad46704ffff60f86100008c220341ecfff94cdf1008b2906700008c42a8000460a64e750001547a00000f1c207afff6701fb298670a584851c8fff8b0404e75594820084e7500000000222f0004670a61da6706202800044e7570004e75222f000461ea6708227affc070214e4f4e75222f0004671261b46710740021420004200890baffa0e6884e757020741f4e75000000000000")
LOCKFIX_REL = (0xF56, 0xFC6, 0xFC2)
YIELD_AT = ((0x2782, "610000fc", "4e714e71"), (0x2874, "610a", "4e71"))
TIMER_AT = 0x283A               # タイマースレッドの「経過 > 0 なら掃除」 (4A85 6F36)          # スレッドを作る _OPEN_PR に積む SR (move.w #0,-(sp) = 3F3C 0000)

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, dst, mss, sup, tim, lck, yld = args[0], args[1], 1460, False, False, False, False
    rest = args[2:]
    while rest:
        a = rest.pop(0)
        if a == "--mss":
            mss = int(rest.pop(0))
        elif a == "--super":
            sup = True
        elif a == "--timer":
            tim = True
        elif a == "--lock":
            lck = "lock"
        elif a == "--lockfix":
            lck = "lockfix"
        elif a == "--yield":
            yld = True
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
    if yld:
        for a, old, new in YIELD_AT:
            o = HDR + a
            assert d[o:o + len(old) // 2] == bytes.fromhex(old)
            d[o:o + len(old) // 2] = bytes.fromhex(new)
    if lck:
        text, data, bss, rel = struct.unpack(">IIII", d[12:28])
        a, b = LOCK_AT
        new, newrel = (LOCK_NEW, LOCK_REL) if lck == "lock" else (LOCKFIX_NEW, LOCKFIX_REL)
        assert len(new) == b - a
        assert hashlib.sha256(d[HDR + a:HDR + b]).hexdigest() == LOCK_OLD_SHA256
        d[HDR + a:HDR + b] = new
        r0 = HDR + text + data
        offs = reloc_parse(bytes(d[r0:r0 + rel]))
        offs = sorted([o for o in offs if not a <= o < b] + list(newrel))
        tab = reloc_build(offs)
        d = d[:r0] + tab + d[r0 + rel:]
        d[0x18:0x1C] = struct.pack(">I", len(tab))
    open(dst, "wb").write(d)
    print("patchxip: %s -> %s (MSS %d%s%s%s%s)" % (src, dst, mss, ", supervisor threads" if sup else "",
                                                ", timer 0.1s" if tim else "", ", one yield per loop" if yld else "", (", " + {"lock": "fast lock", "lockfix": "fast lock + fix"}[lck]) if lck else ""))

main()
