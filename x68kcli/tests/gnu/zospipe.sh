#!/usr/bin/env bash
# zos.x のパイプ (デバイス PIPE0〜7) を通した GNU の道具を XEiJ で試す (ZOS=x68k-zos の置き場、既定は隣)。
#   ls | cat: パイプは端末ではないので、ls は 1 行に 1 つ出す (前は段組みになっていた)
#   seq 100000 | head -2: head が先に終わっても、書く側が止まらずに戻る
set -u
cd "$(dirname "$0")"
X=$(cd ../.. && pwd)
ZOS=${ZOS:-$X/../../x68k-zos}
K=$ZOS/kernel/tests
[ -f "$K/xeij_run.sh" ] || { echo "x68k-zos が無い: $ZOS (ZOS=で指定)"; exit 1; }
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-zospipe}; rm -rf "$OUT"; mkdir -p "$OUT"
make -s -C "$ZOS/kernel" zos.x >/dev/null 2>&1; make -s -C "$ZOS/shell/src" >/dev/null 2>&1
G=$X/build/x/gnu
ZOS_OUT=$OUT ZOS_CONFIG='ENVSET = 8192' ZOS_TIMEOUT=200 \
FILES="$ZOS/kernel/zos.x $ZOS/shell/src/zosh.x $G/ls.x $G/cat.x $G/seq.x $G/head.x" \
ZOS_KEYS='waittext A: 60;sleep 3;type ls.x A:/ | cat.x > A:/L.TXT\n;sleep 10;type seq.x 100000 | head.x -2 > A:/H.TXT\n;sleep 40;type echo done > A:/D.TXT\n;sleep 4;type exit\n' \
  "$K/xeij_run.sh" 'ZOS.X -x -o SNAP.TXT A:\ZOSH.X' > "$OUT/run.log" 2>&1
ok=1
B=$OUT/boot
if [ -f "$B/L.TXT" ] && LC_ALL=C tr -d '\r' < "$B/L.TXT" | grep -qx 'HEAD.X' && ! grep -q '  ' "$B/L.TXT"; then echo "ok   ls | cat は 1 行に 1 つ"
else echo "FAIL ls | cat"; head -3 "$B/L.TXT" 2>/dev/null; ok=0; fi
if [ "$(LC_ALL=C tr -d '\r' < "$B/H.TXT" 2>/dev/null | tr '\n' ' ')" = "1 2 " ] && [ -f "$B/D.TXT" ]; then echo "ok   seq | head -2 が戻る"
else echo "FAIL seq | head -2"; ok=0; fi
[ $ok = 1 ] && echo PASS || exit 1
