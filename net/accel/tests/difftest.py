#!/usr/bin/env python3
"""Pico 側の計算本体 (accel_tool = accel_ops.c + Monocypher、記録形式込み) を、信頼できる参照
(python cryptography) と差分比較する。crypto68k/tests/difftest.py と同じ発想。
usage: difftest.py ACCEL_TOOL [N]
"""
import sys, subprocess, random
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives import serialization
from cryptography.exceptions import InvalidSignature
RAW = serialization.Encoding.Raw
PUB = serialization.PublicFormat.Raw

tool = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
random.seed(20261022)
fails = 0


def run(*a): return subprocess.run([tool, *a], capture_output=True, text=True).stdout.strip()
def rh(n): return bytes(random.randrange(256) for _ in range(n)).hex()
def check(name, cond):
    global fails
    print(("PASS " if cond else "FAIL ") + name)
    if not cond: fails += 1


check("ping -> version 1, caps 7 (verify|x25519|sign)", run("ping") == "1 7")

# --- RFC 7748 section 5.2 (scalarmult) / 6.1 (Alice keygen + shared) ---
check("RFC7748 5.2 X25519", run("mul", "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                                 "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
      == "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
alice_sk = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"
alice_pk = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"
bob_pk = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"
shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
check("RFC7748 6.1 keygen (base)", run("base", alice_sk) == alice_pk)
check("RFC7748 6.1 shared", run("mul", alice_sk, bob_pk) == shared)

# --- RFC 8032 7.1 TEST 1..3 (sign + verify) ---
rfc8032 = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]
for i, (seed, pk, msg, sig) in enumerate(rfc8032, 1):
    check("RFC8032 7.1 TEST %d sign" % i, run("edsign", seed, pk, msg) == sig)
    check("RFC8032 7.1 TEST %d verify" % i, run("edverify", pk, sig, msg) == "1")

# --- X25519 ランダム: cryptography を正解に ---
bad = 0
for _ in range(N):
    s = rh(32); p = rh(32)
    sk = X25519PrivateKey.from_private_bytes(bytes.fromhex(s))
    try:
        truth = sk.exchange(X25519PublicKey.from_public_bytes(bytes.fromhex(p))).hex()
    except ValueError:
        continue            # 全零の共有秘密は cryptography が弾く (dropbear も弾く)
    if run("mul", s, p) != truth: bad += 1
check("X25519 random == cryptography (%d)" % N, bad == 0)
bad = 0
for _ in range(min(N, 100)):
    s = rh(32)
    truth = X25519PrivateKey.from_private_bytes(bytes.fromhex(s)).public_key().public_bytes(RAW, PUB).hex()
    if run("base", s) != truth: bad += 1
check("X25519 base keygen == cryptography", bad == 0)

# --- ed25519 ランダム (メッセージ長 0..1024、上限ちょうども含む) ---
bad_s = bad_v = 0
lens = [0, 1, 32, 64, 200, 255, 256, 1023, 1024] + [random.randrange(0, 1025) for _ in range(max(20, N // 4))]
for n in lens:
    seed = bytes.fromhex(rh(32))
    k = Ed25519PrivateKey.from_private_bytes(seed)
    pk = k.public_key().public_bytes(RAW, PUB).hex()
    msg = rh(n)
    tsig = k.sign(bytes.fromhex(msg)).hex()
    if run("edsign", seed.hex(), pk, msg) != tsig: bad_s += 1
    if run("edverify", pk, tsig, msg) != "1": bad_v += 1
check("ed25519 sign == cryptography (%d msgs, 0..1024 bytes)" % len(lens), bad_s == 0)
check("ed25519 verify ok (%d msgs)" % len(lens), bad_v == 0)

# 改ざんした署名 / メッセージ / 鍵は弾く
seed = bytes.fromhex(rh(32)); k = Ed25519PrivateKey.from_private_bytes(seed)
pk = k.public_key().public_bytes(RAW, PUB).hex(); msg = rh(40)
sig = bytearray(k.sign(bytes.fromhex(msg)))
sig2 = bytearray(sig); sig2[10] ^= 1
check("ed25519 rejects tampered signature", run("edverify", pk, sig2.hex(), msg) == "0")
msg2 = bytearray(bytes.fromhex(msg)); msg2[3] ^= 0x80
check("ed25519 rejects tampered message", run("edverify", pk, sig.hex(), msg2.hex()) == "0")
pk2 = bytearray(bytes.fromhex(pk)); pk2[0] ^= 1
check("ed25519 rejects wrong key", run("edverify", pk2.hex(), sig.hex(), msg) == "0")

print("\n%d failure(s)" % fails)
sys.exit(1 if fails else 0)
