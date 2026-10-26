#!/usr/bin/env bash
# XEiJ で curprobe.x を動かし、端末の間に Human68k のカーソルが戻らないかを見る。
#   tests/xeij_curprobe.sh 出力ディレクトリ [機種...]   (既定: XVI Compact X68030 Hybrid。Hybrid は IPLROM 1.6)
# 機種ごとに 出力ディレクトリ/<機種>.txt に結果。全部 PASS なら 0。
set -u
OUT=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1; [ -n "$OUT" ] || exit 1
shift
MODELS=${*:-XVI Compact X68030 Hybrid}
HERE=$(cd "$(dirname "$0")/.." && pwd)
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
JAVA=java
[ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
make -C "$HERE" -s tests/curprobe.x || exit 1
rc=0
for M in $MODELS; do
  B="$OUT/$M/boot"; T="$OUT/$M/tmp"
  rm -rf "$OUT/$M"; mkdir -p "$B" "$T"
  cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$HERE/tests/curprobe.x" "$B/"
  printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nSHELL = A:\\COMMAND.X\r\n' > "$B/CONFIG.SYS"
  printf 'ECHO OFF\r\nA:\\CURPROBE.X A:\\R.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n' > "$B/AUTOEXEC.BAT"
  "$JAVA" -Djava.io.tmpdir="$T" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
    -model="$M" -boot="$B" -flrinformed=on > "$OUT/$M/xeij.log" 2>&1 &
  PID=$!
  for i in $(seq 1 120); do [ -f "$B/DONE.TXT" ] && break; sleep 1; done
  kill $PID 2>/dev/null; wait $PID 2>/dev/null
  if [ -f "$B/R.TXT" ]; then tr -d '\r' < "$B/R.TXT" > "$OUT/$M.txt"; else echo "no result" > "$OUT/$M.txt"; fi
  echo "== $M"; cat "$OUT/$M.txt"
  grep -qx PASS "$OUT/$M.txt" || rc=1
done
exit $rc
