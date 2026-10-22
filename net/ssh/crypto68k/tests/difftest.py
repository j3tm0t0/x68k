#!/usr/bin/env python3
"""ref (元 M) と fast (M_fast) を、信頼できる参照 (python cryptography) と差分比較する。
xiptest と同じ発想で、RFC ベクタ + 多数のランダム入力で ref/fast の両方を正解と照合する。
usage: difftest.py REF_BIN FAST_BIN [N]
"""
import sys, subprocess, random
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives import serialization
RAW = serialization.Encoding.Raw
PUB = serialization.PublicFormat.Raw
PRV = serialization.PrivateFormat.Raw

ref, fast = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 200
random.seed(20251022)
fails = 0


def run(b, *a): return subprocess.run([b, *a], capture_output=True, text=True).stdout.strip()
def rh(n): return bytes(random.randrange(256) for _ in range(n)).hex()
def check(name, cond):
    global fails
    print(("PASS " if cond else "FAIL ") + name)
    if not cond: fails += 1


def x25519_ref(scalar_hex, point_hex):
    sk = X25519PrivateKey.from_private_bytes(bytes.fromhex(scalar_hex))
    pub = X25519PublicKey.from_public_bytes(bytes.fromhex(point_hex))
    return sk.exchange(pub).hex()


# --- RFC 7748 固定ベクタ ---
rfc_s = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"
rfc_u = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"
rfc_o = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"
check("RFC7748 X25519 ref", run(ref, "mul", rfc_s, rfc_u) == rfc_o)
check("RFC7748 X25519 fast", run(fast, "mul", rfc_s, rfc_u) == rfc_o)

# --- X25519 ランダム: cryptography を正解に ref/fast を照合 ---
bad_ref = bad_fast = bad_rf = 0
for _ in range(N):
    s = rh(32); p = rh(32)
    truth = x25519_ref(s, p)
    r = run(ref, "mul", s, p); f = run(fast, "mul", s, p)
    if r != truth: bad_ref += 1
    if f != truth: bad_fast += 1
    if r != f: bad_rf += 1
check("X25519 random ref==cryptography (%d)" % N, bad_ref == 0)
check("X25519 random fast==cryptography (%d)" % N, bad_fast == 0)
check("X25519 random ref==fast", bad_rf == 0)

# base-point keygen 一致
bad = 0
for _ in range(min(N, 100)):
    s = bytes.fromhex(rh(32))
    truth = X25519PrivateKey.from_private_bytes(s).public_key().public_bytes(RAW, PUB).hex()
    if run(ref, "base", s.hex()) != truth or run(fast, "base", s.hex()) != truth: bad += 1
check("X25519 base keygen ref/fast==cryptography", bad == 0)

# --- ed25519 ランダム: cryptography を正解に sign/verify を照合 ---
bad_sr = bad_sf = bad_vr = bad_vf = 0
for _ in range(max(40, N // 4)):
    seed = bytes.fromhex(rh(32))
    k = Ed25519PrivateKey.from_private_bytes(seed)
    pub = k.public_key().public_bytes(RAW, PUB).hex()
    msg = rh(random.randrange(0, 64))
    tsig = k.sign(bytes.fromhex(msg)).hex()
    if run(ref, "edsign", seed.hex(), pub, msg) != tsig: bad_sr += 1
    if run(fast, "edsign", seed.hex(), pub, msg) != tsig: bad_sf += 1
    if run(ref, "edverify", pub, tsig, msg) != "1": bad_vr += 1
    if run(fast, "edverify", pub, tsig, msg) != "1": bad_vf += 1
check("ed25519 sign ref==cryptography", bad_sr == 0)
check("ed25519 sign fast==cryptography", bad_sf == 0)
check("ed25519 verify ref ok", bad_vr == 0)
check("ed25519 verify fast ok", bad_vf == 0)

# ed25519 negative: 改ざんした署名は弾く
bad = 0
seed = bytes.fromhex(rh(32)); k = Ed25519PrivateKey.from_private_bytes(seed)
pub = k.public_key().public_bytes(RAW, PUB).hex(); msg = rh(10)
sig = bytearray(k.sign(bytes.fromhex(msg))); sig[10] ^= 1
if run(ref, "edverify", pub, sig.hex(), msg) == "1" or run(fast, "edverify", pub, sig.hex(), msg) == "1":
    bad = 1
check("ed25519 rejects tampered signature", bad == 0)

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
