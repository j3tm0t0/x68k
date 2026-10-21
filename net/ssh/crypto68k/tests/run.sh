#!/usr/bin/env bash
# crypto68k の全テスト: ホスト差分 (C 版 M_fast) + asm の Unicorn 照合 + asm 経路の差分。
# 要 venv (pyte/pynacl/cryptography/unicorn)。既定は term/vt100/tests/venv を流用。
set -euo pipefail
cd "$(dirname "$0")"
VENV=${VENV:-../../../../term/vt100/tests/venv}
PY="$VENV/bin/python"
[ -x "$PY" ] || { echo "venv がない: $VENV (pip install pynacl cryptography unicorn)"; exit 1; }
N=${1:-500}

cc -O2 -I.. -o /tmp/ck_ref  x25519_tool.c sha512.c
cc -O2 -I.. -DFAST_FIELD -o /tmp/ck_fast x25519_tool.c sha512.c
cc -O2 -I.. -DFAST_FIELD -DUSE_ASM -DHOST_ASM_STUB -o /tmp/ck_astub x25519_tool.c sha512.c
m68k-xelf-as -m68000 -o /tmp/ck_mul.o ../mul68k.s

echo "== C 版 M_fast を cryptography と差分 =="
"$PY" -I difftest.py /tmp/ck_ref /tmp/ck_fast "$N"
echo "== asm fe_mul16/fe_sqr16 を Unicorn で C 参照と照合 =="
"$PY" -I unicorn_mul.py /tmp/ck_mul.o "$N"
echo "== asm 経路 (norm/reduce) を cryptography と差分 =="
"$PY" -I difftest.py /tmp/ck_ref /tmp/ck_astub "$N"
echo "ALL CRYPTO68K TESTS PASSED"
