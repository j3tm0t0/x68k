#!/usr/bin/env bash
# protprobe.x を XEiJ (X68000 SUPER、68000、主記憶 12MB) で走らせ、結果を表示する。最後の行が OK なら終了コード 0
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/protprobe-test}
make -C "$here" protprobe.x >/dev/null
OUT=$OUT FILES="$here/protprobe.x" TIMEOUT=${TIMEOUT:-120} "$here/../../x68kcli/tests/xeij_hfs.sh" 'PROTPROBE.X > OUT.TXT'
iconv -f CP932 -t UTF-8 "$OUT/boot/OUT.TXT" | tr -d '\r' | tee "$OUT/result.txt"
[ "$(tail -n 1 "$OUT/result.txt")" = OK ]
