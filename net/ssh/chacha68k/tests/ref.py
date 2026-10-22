"""python cryptography で OpenSSH の chacha20-poly1305@openssh.com を計算する (bench.c / csim の正解)。
usage: ref.py INFILE  -> 暗号文 + タグを stdout に (INFILE = 鍵 64 + seq 4 (BE) + 平文)"""
import sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives.poly1305 import Poly1305

def chacha(key, seq, ctr, data):
    nonce = ctr.to_bytes(8, "little") + seq.to_bytes(8, "big")
    return Cipher(algorithms.ChaCha20(key, nonce), mode=None).encryptor().update(data)

def seal(blob):
    k2, k1, seq, pt = blob[:32], blob[32:64], int.from_bytes(blob[64:68], "big"), blob[68:]
    pk = chacha(k2, seq, 0, bytes(32))
    ct = chacha(k1, seq, 0, pt[:4]) + chacha(k2, seq, 1, pt[4:])
    return ct + Poly1305.generate_tag(pk, ct)

if __name__ == "__main__":
    sys.stdout.buffer.write(seal(open(sys.argv[1], "rb").read()))
