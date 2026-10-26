#!/usr/bin/env bash
# XEiJ で crtmode.x 19 を動かし、そのあとの COMMAND.X の表示 (折り返し・スクロール・ファンクションキー行) を PNG にする。
#   tests/xeij_crtmode.sh 出力ディレクトリ [モード (既定 19)]
# スクリーンショットは modetest の ShotAgent.java (XEiJ の画面バッファを PNG に書く) を使う。
set -u
OUT=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1; [ -n "$OUT" ] || exit 1
MODE=${2:-19}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SHOT=$HERE/../modetest/tests/ShotAgent.java
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
JAVA=java; JAVAC=javac; JAR=jar
[ -x /opt/homebrew/opt/openjdk/bin/java ] && { JAVA=/opt/homebrew/opt/openjdk/bin/java; JAVAC=/opt/homebrew/opt/openjdk/bin/javac; JAR=/opt/homebrew/opt/openjdk/bin/jar; }
B="$OUT/boot"; T="$OUT/tmp"
rm -rf "$B" "$T" "$OUT/agent.cmd" "$OUT/shots"; mkdir -p "$B" "$T" "$OUT/shots"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$HERE/crtmode.x" "$B/"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nSHELL = A:\\COMMAND.X\r\n' > "$B/CONFIG.SYS"
LONG=0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
{
  printf "ECHO OFF\r\nPATH A:\\\r\nA:\\CRTMODE.X\r\nA:\\CRTMODE.X %s > A:\\R.TXT\r\n" "$MODE"
  printf 'ECHO %s\r\n' "$LONG"                       # 80 桁を超える行 (折り返し)
  for i in $(seq 1 ${NLINES:-45}); do printf 'ECHO line %s\r\n' "$i"; done   # スクロール
  printf 'DIR A:\\\r\n'
  [ -n "${RESTORE:-}" ] && printf 'A:\\CRTMODE.X 16\r\nECHO back to 16\r\nDIR A:\\\r\n'   # RESTORE=1: crtmode 16 で戻す
  [ -n "${EXTRA:-}" ] && printf '%s' "$EXTRA"   # EXTRA=$'A:\\CRTMODE.X -k off\r\n' など、最後に足す行 (CR LF 入り)
  printf 'COPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"
cd "$OUT"
mkdir -p agent && "$JAVAC" -d agent "$SHOT" && printf 'Premain-Class: ShotAgent\n' > agent/m.txt && (cd agent && "$JAR" cfm ../shotagent.jar m.txt ShotAgent*.class)
"$JAVA" -javaagent:"$OUT/shotagent.jar=$OUT/agent.cmd" -Djava.io.tmpdir="$T" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
  -model="${MODEL:-SUPER}" -boot="$B" -flrinformed=on -usecanvas=off > "$OUT/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
shot() { rm -f "$1"; printf 'shot %s\n' "$1" > "$OUT/agent.cmd"; for i in $(seq 1 40); do [ -s "$1" ] && break; sleep 0.5; done; sleep 0.5; }
for i in $(seq 1 120); do [ -f "$B/DONE.TXT" ] && break; sleep 1; done
sleep 4
shot "$OUT/shots/mode$MODE.png" && echo "shot mode $MODE"
