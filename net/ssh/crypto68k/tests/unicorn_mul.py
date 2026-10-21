#!/usr/bin/env python3
"""Unicorn の 68000 で fe_mul16 / fe_sqr16 (mul68k.s) を走らせ、Python の参照積と照合する。
usage: unicorn_mul.py mul68k.o [N]
"""
import sys, struct, random, subprocess, tempfile, os
from unicorn import Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.m68k_const import UC_M68K_REG_PC, UC_M68K_REG_A7, UC_M68K_REG_D0

obj = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 500

# シンボルオフセットと .text バイナリを得る
nm = subprocess.run(["m68k-xelf-nm", obj], capture_output=True, text=True).stdout
sym = {}
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
binf = tempfile.mktemp(suffix=".bin")
subprocess.run(["m68k-xelf-objcopy", "-O", "binary", "-j", ".text", obj, binf], check=True)
code = open(binf, "rb").read()
os.unlink(binf)

CODE, DATA, STACK = 0x1000, 0x20000, 0x80000
RET = 0x900           # 偶数の番地。ここに来たら停止


def call(fn, r_addr, a_addr, b_addr=None):
    mu = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    mu.mem_map(0x0, 0x100000)
    mu.mem_write(CODE, code)
    mu.mem_write(RET, b"\x4e\x75")  # rts (何でもよい; stop で止める)
    sp = STACK
    args = [RET, r_addr, a_addr] + ([b_addr] if b_addr is not None else [])
    # スタックに retaddr, 引数を big-endian で積む
    for i, val in enumerate(args):
        mu.mem_write(sp + 4 * i, struct.pack(">I", val & 0xffffffff))
    mu.reg_write(UC_M68K_REG_A7, sp)
    mu.emu_start(CODE + sym[fn], RET, count=200000)


def limbs_to_int(limbs):
    return sum(v << (16 * i) for i, v in enumerate(limbs))


def ref_product(a, b):
    P = limbs_to_int(a) * limbs_to_int(b)
    return [(P >> (16 * k)) & 0xffff for k in range(32)]


random.seed(7)
fails = 0
for t in range(N):
    a = [random.randrange(0x10000) for _ in range(16)]
    b = [random.randrange(0x10000) for _ in range(16)]
    mu = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    mu.mem_map(0x0, 0x100000)
    mu.mem_write(CODE, code)
    mu.mem_write(RET, b"\x4e\x75")
    AA, BB, RR = DATA, DATA + 0x40, DATA + 0x80
    for i in range(16):
        mu.mem_write(AA + 2 * i, struct.pack(">H", a[i]))
        mu.mem_write(BB + 2 * i, struct.pack(">H", b[i]))
    sp = STACK
    for i, val in enumerate([RET, RR, AA, BB]):
        mu.mem_write(sp + 4 * i, struct.pack(">I", val))
    mu.reg_write(UC_M68K_REG_A7, sp)
    mu.emu_start(CODE + sym["fe_mul16"], RET, count=500000)
    r = [struct.unpack(">H", mu.mem_read(RR + 2 * k, 2))[0] for k in range(32)]
    if r != ref_product(a, b):
        fails += 1
        if fails <= 2:
            print("MUL MISMATCH at", t)
    # square
    mu2 = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    mu2.mem_map(0x0, 0x100000)
    mu2.mem_write(CODE, code)
    mu2.mem_write(RET, b"\x4e\x75")
    for i in range(16):
        mu2.mem_write(AA + 2 * i, struct.pack(">H", a[i]))
    for i, val in enumerate([RET, RR, AA]):
        mu2.mem_write(sp + 4 * i, struct.pack(">I", val))
    mu2.reg_write(UC_M68K_REG_A7, sp)
    mu2.emu_start(CODE + sym["fe_sqr16"], RET, count=500000)
    rs = [struct.unpack(">H", mu2.mem_read(RR + 2 * k, 2))[0] for k in range(32)]
    if rs != ref_product(a, a):
        fails += 1
        if fails <= 4:
            print("SQR MISMATCH at", t)

print("%s: %d/%d ok, %d fail" % ("fe_mul16/fe_sqr16", N - (fails and N or 0) if False else N, N, fails))
print("PASS" if fails == 0 else "FAIL")
sys.exit(1 if fails else 0)
