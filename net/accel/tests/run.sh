#!/usr/bin/env bash
# net/accel のホストテスト:
#   1. 記録形式 (accel_proto.h) の組み立て / 解読の往復
#   2. Pico 側の計算本体 (accel_ops.c + Monocypher) を RFC 7748 / 8032 のベクタと python cryptography で差分
#   3. ファームウェア側に置いたコピーが一致しているか (ある場合)
# 要 python venv (cryptography)。VENV で場所を指定。
set -euo pipefail
cd "$(dirname "$0")"
VENV=${VENV:-./venv}
PY="$VENV/bin/python"
[ -x "$PY" ] || { echo "venv がない: $VENV (python3 -m venv venv && venv/bin/pip install cryptography)"; exit 1; }
N=${1:-300}
OUT=${TMPDIR:-/tmp}/accel_tool
cc -O2 -Wall -I.. -I../monocypher -o "$OUT" accel_tool.c ../accel_ops.c ../monocypher/monocypher.c ../monocypher/monocypher-ed25519.c

echo "== 記録形式の往復 =="
"$OUT" roundtrip 2000
echo "== npd.sys の ETD 20..24 と Pico 側をつないだ模擬 =="
cc -O1 -Wall -Wno-unused-function -I.. -I../monocypher -o "$OUT-npdsim" npdsim.c ../accel_ops.c ../monocypher/monocypher.c ../monocypher/monocypher-ed25519.c
"$OUT-npdsim"
echo "== Monocypher 経由の計算を cryptography と差分 =="
"$PY" -I difftest.py "$OUT" "$N"
FW=${FW:-../../../../../yunkya2/x68kzremotedrv-accel/src}
if [ -d "$FW" ]; then
  echo "== ファームウェアのコピーとの一致 =="
  for f in accel_proto.h accel_ops.c accel_ops.h; do cmp "../$f" "$FW/$f"; done
  for f in monocypher.c monocypher.h monocypher-ed25519.c monocypher-ed25519.h; do cmp "../monocypher/$f" "$FW/monocypher/$f"; done
  echo "一致"
fi
echo "ALL ACCEL HOST TESTS PASSED"
