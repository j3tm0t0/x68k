"""cp68k の chacha20-poly1305@openssh.com を python cryptography (ref.py) と突き合わせる。
usage: difftest.py N CMD...   CMD の後ろに INFILE OUTFILE を付けて実行し、OUTFILE を正解と比べる。
  例: difftest.py 300 ./hosttool_wrap      /  difftest.py 100 csim sim.bin sim.nm
長さは 4..35000 の乱数 (小さい長さ・16/64 の境目を多めに)。失敗したら入力を残して止まる。"""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref import seal

def lengths(n):
    fixed = [4, 5, 15, 16, 17, 20, 31, 32, 33, 63, 64, 65, 67, 68, 69, 127, 128, 129, 132, 1000, 4096, 16384, 32768, 35000]
    out = fixed[:n]
    while len(out) < n:
        out.append(random.choice([random.randint(4, 200), random.randint(4, 35000)]))
    return out

def main():
    n, cmd = int(sys.argv[1]), sys.argv[2:]
    d = tempfile.mkdtemp()
    fin, fout = os.path.join(d, "in"), os.path.join(d, "out")
    bad = 0
    for i, ln in enumerate(lengths(n)):
        blob = os.urandom(64) + random.getrandbits(32).to_bytes(4, "big") + os.urandom(ln)
        open(fin, "wb").write(blob)
        r = subprocess.run(cmd + [fin, fout], capture_output=True)
        got = open(fout, "rb").read() if os.path.exists(fout) else b""
        if r.returncode != 0 or got != seal(blob):
            print("FAIL len", ln, r.stderr.decode()[-300:], "input kept in", fin)
            bad += 1
            break
        os.remove(fout)
    print("%d/%d ok" % (i + 1 - bad, i + 1))
    sys.exit(1 if bad else 0)

main()
