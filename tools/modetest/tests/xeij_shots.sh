#!/usr/bin/env bash
# XEiJ で modetest.x を動かし、モードごとに画面を PNG にする。
#   tests/xeij_shots.sh [-m MODEL] [-x] 出力ディレクトリ モード番号...
# XEiJ の kbdDoCapture は黒い画像しか出さない環境があるので、ShotAgent.java (画面の BufferedImage を PNG に書く) を使う。
# 他の XEiJ (別の担当の試験) には触らない: 自分用の java.io.tmpdir と -boot ディレクトリを使う。
set -u
MODEL=SUPER; EXT=
while getopts m:x o; do case $o in m) MODEL=$OPTARG;; x) EXT=-x;; *) exit 2;; esac; done
shift $((OPTIND-1))
OUT=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1; [ -n "$OUT" ] || exit 1; shift
HERE=$(cd "$(dirname "$0")/.." && pwd); TD=$HERE/tests
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
JAVA=java; JAVAC=javac
[ -x /opt/homebrew/opt/openjdk/bin/java ] && { JAVA=/opt/homebrew/opt/openjdk/bin/java; JAVAC=/opt/homebrew/opt/openjdk/bin/javac; JAR=/opt/homebrew/opt/openjdk/bin/jar; }
JAR=${JAR:-jar}
B="$OUT/boot"; T="$OUT/tmp"
rm -rf "$B" "$T" "$OUT/agent.cmd"; mkdir -p "$B" "$T"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$HERE/modetest.x" "$B/"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nSHELL = A:\\COMMAND.X\r\n' > "$B/CONFIG.SYS"
printf 'ECHO OFF\r\nPATH A:\\\r\nECHO ORIGINAL TEXT BEFORE MODETEST\r\nA:\\MODETEST.X -l %s > A:\\LIST.TXT\r\nA:\\MODETEST.X %s %s\r\nCOPY CONFIG.SYS DONE.TXT\r\n' "$EXT" "$EXT" "$1" > "$B/AUTOEXEC.BAT"
cd "$OUT"
mkdir -p agent && "$JAVAC" -d agent "$TD/ShotAgent.java" && printf 'Premain-Class: ShotAgent\n' > agent/m.txt && (cd agent && "$JAR" cfm ../shotagent.jar m.txt ShotAgent*.class)
"$JAVA" -javaagent:"$OUT/shotagent.jar=$OUT/agent.cmd" -Djava.io.tmpdir="$T" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
  -model="$MODEL" -boot="$B" -flrinformed=on -usecanvas=off -pastepipe=on -pipeencoding=utf8 > "$OUT/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
ctl() { printf '%s\n' "$1" > "$T/XEiJControl"; sleep "${2:-0.3}"; }
shot() { rm -f "$1"; printf 'shot %s\n' "$1" > "$OUT/agent.cmd"
  for i in $(seq 1 40); do [ -s "$1" ] && break; sleep 0.5; done; sleep 0.5; }
for i in $(seq 1 60); do [ -p "$T/XEiJControl" ] && break; sleep 1; done
for i in $(seq 1 120); do [ -f "$B/LIST.TXT" ] && break; sleep 1; done
sleep "${BOOTWAIT:-6}"
mkdir -p "$OUT/shots"
cur=$1
for m in "$@"; do
  while [ "$cur" -lt "$m" ]; do ctl "typekey space" "${STEPWAIT:-4}"; cur=$((cur+1)); done
  sleep "${SHOTWAIT:-5}"
  shot "$OUT/shots/mode$m.png" && echo "mode $m shot"
done
sleep 1; ctl "typekey q"; sleep 4; shot "$OUT/shots/after.png"
echo done
