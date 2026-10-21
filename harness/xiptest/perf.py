"""ロック → アンロックの 1 組にかかる命令数を、元と lock.bin で比べる (使用中の項目が n 個あるとき)"""
import random, struct, sys
sys.path.insert(0, '.')
from m68emu import Emu, Thread
from unicorn import UC_HOOK_CODE
from difflock import images, LOAD, LOCK, UNLOCK

orig, new = images(sys.argv[1], open('lock.bin', 'rb').read())
for n in (1, 4, 16, 30):
    res = []
    for img in (orig, new):
        e = Emu(bytes(LOAD) + img)
        cnt = [0]
        e.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u: cnt.__setitem__(0, cnt[0] + 1))
        t = Thread(e, 0x70000)
        keys = list(range(0x1000, 0x1000 + n))
        for k in keys:                       # n 個を割り当てておく (表に n 個のキー)
            t.call(LOAD + LOCK, k); t.call(LOAD + UNLOCK, k)
        rnd = random.Random(1)
        cnt[0] = 0
        reps = 200
        for _ in range(reps):                # 同じキーを続けて使うことが多い (ソケット単位)
            k = keys[-1] if rnd.random() < 0.7 else rnd.choice(keys)
            t.call(LOAD + LOCK, k); t.call(LOAD + UNLOCK, k)
        res.append(cnt[0] / reps)
    print('keys %2d: orig %6.1f  new %6.1f  命令/組 (%.1f 倍)' % (n, res[0], res[1], res[0] / res[1]))
