#!/usr/bin/env bash
# zosh (x68k-zos の shell/src/zosh.x) から GNU の道具を動かす試験。COMMAND.X では見えないこと
# (2>&1 のエラーの形、終了コード、子が終わったあとも親の出力が生きているか、パイプ、カレントディレクトリ) を見る。
# 使い方: ZOSH=…/zosh.x tests/gnu/zosh.sh   (既定は隣に置いた x68k-zos の shell/src/zosh.x)
set -u
cd "$(dirname "$0")"
T=$PWD; X=$(cd ../.. && pwd)
ZOSH=${ZOSH:-$X/../../x68k-zos/shell/src/zosh.x}
[ -f "$ZOSH" ] || { echo "zosh.x が無い: $ZOSH (ZOSH=で指定)"; exit 1; }
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-gnu-zosh}; rm -rf "$OUT"; mkdir -p "$OUT/in"
printf 'banana 3\r\napple 10\r\ncherry 2\r\napple 10\r\n' > "$OUT/in/in.txt"
cp t_zosh.zsh "$OUT/in/T.ZSH"
XEIJCOMMAND=$X/tools/xeij/data/COMMAND.X   # popen (sdiff・diff3) が COMMAND.X /C を使う (起動の A:\ にも XEiJ のものがある)
FILES="$(ls "$X"/build/x/gnu/*.x | tr '\n' ' ') $ZOSH $XEIJCOMMAND $OUT/in/T.ZSH $OUT/in/in.txt" OUT=$OUT TIMEOUT=${TIMEOUT:-300} \
  "$X/tests/xeij_hfs.sh" 'ZOSH.X T.ZSH > OUT.TXT' > "$OUT/run.log" 2>&1 || cat "$OUT/run.log"
if diff <(tr -d '\r' < "$OUT/boot/OUT.TXT") t_zosh.exp > "$OUT/diff.txt"; then echo PASS; else echo FAIL; cat "$OUT/diff.txt"; exit 1; fi
