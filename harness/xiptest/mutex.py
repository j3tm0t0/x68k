"""ロックの排他の検査: スレッドが割り込みでどこでも切り替わる (乱数の命令数ごと) 条件で、
同じキーを 2 本のスレッドが同時に持つ (ロックから戻ってからアンロックを呼ぶまで) ことがないかを調べる。

  python3 mutex.py XIP.X [回数] [当てた XIP.X]
"""
import random, struct, sys
sys.path.insert(0, '.')
from m68emu import Emu, Thread
from difflock import image_of, LOAD, LOCK, UNLOCK, IRQFLAG


def run(img, seed, nthreads=3, nops=400, nkeys=3):
    rnd = random.Random(seed)
    e = Emu(bytes(LOAD) + img)
    e.wr(LOAD + IRQFLAG, bytes(4))
    keys = [0x100 + i for i in range(nkeys)]
    ths = [Thread(e, 0x70000 - i * 0x4000) for i in range(nthreads)]
    st = [None] * nthreads            # None: 何もしていない / ('lock', k) 実行中 / ('held', k) / ('unlock', k)
    holders = {k: set() for k in keys}
    for step in range(nops):
        i = rnd.randrange(nthreads)
        t = ths[i]
        e.steps_left = rnd.randint(1, 60)   # この命令数で割り込まれたことにする
        if st[i] is None or st[i][0] == 'held':
            if st[i] is None:
                k = rnd.choice(keys)
                st[i] = ('lock', k)
                ev = t.call(LOAD + LOCK, k)
            else:
                k = st[i][1]
                st[i] = ('unlock', k)
                holders[k].discard(i)
                ev = t.call(LOAD + UNLOCK, k)
        else:
            if t.pending_skip:
                t.skip()
            ev = t.run()
        t.pending_skip = ev[0] == 'yield'    # _CHANGE_PR で止まった: 次は飛ばして続ける
        if ev[0] == 'done':
            op, k = st[i]
            if op == 'lock':
                holders[k].add(i)
                st[i] = ('held', k)
                if len(holders[k]) > 1:
                    return 'violation step %d key %x holders %s' % (step, k, sorted(holders[k]))
            else:
                st[i] = None
        elif ev[0] not in ('yield', 'preempt', 'steps'):
            return 'error %s' % (ev,)
    return None


if __name__ == '__main__':
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    for path in [sys.argv[1]] + sys.argv[3:]:
        img = image_of(path)
        bad = 0
        first = None
        for seed in range(n):
            r = run(img, seed)
            if r:
                bad += 1
                first = first or (seed, r)
        print('%s: %d/%d runs violated mutual exclusion%s' % (path, bad, n, ' (first: seed %d %s)' % first if first else ''))
