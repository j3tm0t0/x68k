"""xip.x のロック (0xF4E ロック / 0xFE8 参照 / 0x100E アンロック) を、元と lock.bin で比べる差分テスト。

  python3 difflock.py XIP.X [回数] [当てた XIP.X]   (3 つ目を渡すと lock.bin ではなく、patchxip.py --lock の結果と比べる)

両方の xip.x を 0x10000 に置いて再配置し、同じ乱数で作った操作の並びを、何本かのスレッドで実行する。
スレッドは _CHANGE_PR (譲る) でだけ切り替わる (協調的)。各手順の結果 (戻り値、表の中身、譲ったか、IOCS を呼んだか)
が一致すること、元が触らないレジスタを新しい方も壊さないことを確かめる。
"""
import random, struct, sys
sys.path.insert(0, '.')
from m68emu import Emu, Thread
from image import load
import reloc

LOAD = 0x10000
TABLE, IRQFLAG, MASTER = 0x1547a, 0x19ab4, 0x15476
LOCK, LOOKUP, UNLOCK = 0xf4e, 0xfe8, 0x100e
BLOCK = (0xf4e, 0x1034)
NEWREL = {l.split()[0]: int(l.split()[1], 16) for l in open('lock.rel')}


def relocated(img, offs):
    b = bytearray(img)
    for o in offs:
        v = struct.unpack('>I', b[o:o + 4])[0]
        b[o:o + 4] = struct.pack('>I', v + LOAD)
    return bytes(b)


def image_of(path):
    """xip.x を、その再配置表のとおりに LOAD へ再配置した像"""
    img, x, (t, d, bss, r) = load(path)
    return relocated(img, reloc.parse(x[0x40 + t + d:0x40 + t + d + r]))


def images(path, newbin):
    img, x, (t, d, bss, r) = load(path)
    offs = reloc.parse(x[0x40 + t + d:0x40 + t + d + r])
    new = bytearray(img)
    new[BLOCK[0]:BLOCK[1]] = newbin
    noffs = sorted([o for o in offs if not BLOCK[0] <= o < BLOCK[1]] + list(NEWREL.values()))
    return relocated(img, offs), relocated(bytes(new), noffs)


def run(img, seed, nthreads, nops, keys, paired=False):
    rnd = random.Random(seed)
    held = [[] for _ in range(nthreads)]
    e = Emu(bytes(LOAD) + img)
    ths = [Thread(e, 0x70000 - i * 0x4000) for i in range(nthreads)]
    state = ['idle'] * nthreads       # idle / wait (譲って止まっている)
    trace = []
    for step in range(nops):
        i = rnd.randrange(nthreads)
        t = ths[i]
        if state[i] == 'wait':
            t.skip()
            ev = t.run()
            op = 'cont'
        else:
            op = rnd.choice(['lock', 'lock', 'unlock', 'unlock', 'lookup', 'irq'])
            k = rnd.choice(keys)
            if paired and held[i] and rnd.random() < 0.7:
                op, k = 'unlock', held[i].pop()   # 実際の使い方: 取ったものを放す
            elif paired and op == 'unlock':
                op = 'lock'
            if paired and op == 'lock':
                held[i].append(k)
            e.wr(LOAD + IRQFLAG, struct.pack('>I', 1 if op == 'irq' else 0))
            entry = {'lock': LOCK, 'irq': LOCK, 'unlock': UNLOCK, 'lookup': LOOKUP}[op]
            before = {('d%d' % j): 0xd0d0d000 + j for j in range(8)} | {('a%d' % j): 0xa0a0a000 + j for j in range(7)}
            ev = t.call(LOAD + entry, k)
            t.before = before
            op = '%s(%x)' % (op, k)
        if ev[0] == 'iocs':            # panic の表示: 飛ばして続ける
            trace.append((step, i, op, 'iocs', ev[2]))
            t.skip()
            ev = t.run()
        if ev[0] == 'yield':
            state[i] = 'wait'
            rec = (step, i, op, 'yield')
        elif ev[0] == 'done':
            state[i] = 'idle'
            regs = t.regs()
            changed = frozenset(r for r, v in t.before.items() if regs[r] != v)
            rec = (step, i, op, 'done', ev[1], changed)
        else:
            rec = (step, i, op) + tuple(ev)
        tab = e.rd(LOAD + MASTER, 4 + 256)
        trace.append(rec + (tab,))
    return trace


def compare(a, b):
    for x, y in zip(a, b):
        if x[3] == 'done' and y[3] == 'done':
            # d0 と表は一致。新しい方が変えたレジスタは、元も変えたものか、どの m68k の呼び出し規約でも
            # 使い捨ての d0 / d1 / a0 / a1 だけ (d2 は gcc では呼ばれた側が保存するので厳しく見る)
            if x[4] != y[4] or x[6] != y[6] or not (y[5] - {'a7'}) <= (x[5] | {'d0', 'd1', 'a0', 'a1'}):
                return x, y
        elif x != y:
            return x, y
    return None


if __name__ == '__main__':
    orig, new = images(sys.argv[1], open('lock.bin', 'rb').read())
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    if len(sys.argv) > 3:
        new = image_of(sys.argv[3])
        # MSS や --timer の書き換えはロックと関係ないので、比べる像の元の側にも当てておく必要はない (表の中身だけを比べる)
    stats = {}
    for seed in range(n):
        rnd = random.Random(seed * 7919)
        nth = rnd.choice([1, 2, 3, 4])
        keys = [0] + [rnd.randrange(1, 1 << 32) for _ in range(rnd.choice([2, 5, 20, 40]))]
        paired = seed % 2 == 1
        a = run(orig, seed, nth, 300, keys, paired)
        b = run(new, seed, nth, 300, keys, paired)
        for r in a:
            stats[r[3]] = stats.get(r[3], 0) + 1
        bad = compare(a, b)
        if bad or len(a) != len(b):
            print('MISMATCH seed', seed)
            print(' orig:', bad[0][:6] if bad else len(a))
            print(' new :', bad[1][:6] if bad else len(b))
            if bad:
                print(' orig tab', bad[0][-1].hex())
                print(' new  tab', bad[1][-1].hex())
            sys.exit(1)
    print('ok: %d runs, events %s' % (n, stats))
