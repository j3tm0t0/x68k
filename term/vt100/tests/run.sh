#!/usr/bin/env bash
# vt100 エミュレータの比較試験。
#   Mac 上: スタブ版 vthost (パーサ・スクロール・属性) の出力を pyte (参照レンダラ) と比べる。
#   実機:   vtcat.x をイメージに入れて XEiJ で動かし、書き出した .out を同じく pyte と比べる
#           (下の RUN_TARGET の手順。ここでは Mac 側だけ自動化する)。
# 使い方: tests/run.sh [COLS ROWS]   (既定 80x24)
set -euo pipefail
cd "$(dirname "$0")/.."
COLS=${1:-80}; ROWS=${2:-24}
PY=tests/venv/bin/python
[ -x "$PY" ] || { echo "pyte の venv がない。tests/venv を作って pyte を入れること:"; \
  echo "  python3 -m venv tests/venv && tests/venv/bin/pip install pyte"; exit 1; }

make -s vthost
fail=0
for rec in tests/fixtures/*.rec; do
  name=$(basename "$rec" .rec)
  ./vthost -s ${COLS}x${ROWS} "$rec" "/tmp/vt_mine_$name.txt"
  $PY -I tests/pyte_ref.py "$rec" "/tmp/vt_pyte_$name.txt" "$COLS" "$ROWS"
  if diff -q "/tmp/vt_mine_$name.txt" "/tmp/vt_pyte_$name.txt" >/dev/null; then
    echo "PASS  $name"
  else
    echo "FAIL  $name  (vthost vs pyte)"
    diff "/tmp/vt_mine_$name.txt" "/tmp/vt_pyte_$name.txt" | head -12
    fail=1
  fi
done
exit $fail
