#!/usr/bin/env bash
# rcc.x を XEiJ + picosim (tis.x -p) で試す: Mac の rccd.py (127.0.0.1:7790、Z からは 192.168.1.1) でコンパイルし、
# できた .x を Z で動かす。つながらないとき (RCC_LOCAL を動かす / 空なら 255 で終わる)、gcc のエラー (.x を作らない) も見る
# 使い方: net/rcc/tests/xeij_rcc.sh    (要るもの: x68kcli/tools の XEiJ と agent、harness/picosim/build/picosim (TSP 入り))
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
TIS=$TOP/net/tistack
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
AGENT=$TOP/x68kcli/tools/xeijagent.jar
OUT=${RCC_OUT:-${TMPDIR:-/tmp}/rcctest}
BRIDGE=${BRIDGE:-26978} PORT=${PORT:-7790}
B="$OUT/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
make -s && make -s -C "$TIS" && make -s -C "$TOP/net/npd" || { echo "FAIL build"; exit 1; }
rm -rf "$OUT"; mkdir -p "$B" "$OUT/tmp"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp "$TIS/tis.x" "$B/TIS.X"; cp "$TOP/net/npd/npd.sys" "$B/NPD.SYS"; cp "$TOP/net/npd/npd.x" "$B/NPD.X"; cp rcc.x "$B/RCC.X"
printf '#include <stdio.h>\r\n#include "msg.h"\r\nint main(void) { printf("%%s\\n", MSG); return 0; }\r\n' > "$B/HELLO.C"
printf '#define MSG "hello from rccd"\r\n' > "$B/MSG.H"
printf 'int main(void) { return syntax error; }\r\n' > "$B/BAD.C"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nPROCESS = 16 10 20\r\nSHELL = A:\\COMMAND.X\r\nDEVICE = \\NPD.SYS\r\n' > "$B/CONFIG.SYS"
{
  printf 'ECHO OFF\r\nPATH A:\\\r\n'
  printf 'TIS.X -p > TIS.TXT\r\nNPD.X 192.168.1.68 > NPD.TXT\r\n'
  printf 'SET RCC_HOST=192.168.1.1:%d\r\n' "$PORT"
  printf 'RCC.X -O2 HELLO.C -o HELLO.X\r\nHELLO.X > HELLO.TXT\r\n'
  printf 'RCC.X BAD.C -o BAD.X\r\n'
  printf 'SET RCC_HOST=192.168.1.1:%d\r\nSET RCC_LOCAL=\r\nRCC.X HELLO.C -o NONE.X\r\n' $((PORT + 1))
  printf 'SET RCC_LOCAL=COPY HELLO.C LOCAL.C%%.0s\r\nRCC.X HELLO.C -o LOCAL.X\r\n'
  printf 'TIS.X -s > STAT.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"
pids=()
cleanup() { for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
python3 -I rccd.py --port "$PORT" --allow 127.0.0.0/8 > "$OUT/rccd.log" 2>&1 & pids+=($!)
TSP_DNS=192.168.1.3 "$TOP/harness/picosim/build/picosim" -l 127.0.0.1:$BRIDGE -s 10 > "$OUT/picosim.log" 2>&1 & pids+=($!)
sleep 1
"$JAVA" -javaagent:"$AGENT=$OUT/tmp/agent.cmd" -Dpicosim=127.0.0.1:$BRIDGE -Dpicosim.ids=6 -Djava.io.tmpdir="$OUT/tmp" \
  -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off \
  > "$OUT/xeij.log" 2>&1 & pids+=($!)
XPID=$!
for t in $(seq 1 ${RCC_TIMEOUT:-300}); do [ -f "$B/DONE.TXT" ] && break; kill -0 $XPID 2>/dev/null || break; sleep 1; done
fails=0
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
ok "rcc.x: hello.c (+ msg.h from the same directory) compiled remotely, HELLO.X runs" "grep -q 'hello from rccd' '$B/HELLO.TXT' 2>/dev/null"
ok "rcc.x: a compile error leaves no BAD.X" "[ ! -f '$B/BAD.X' ]"
ok "rcc.x: server unreachable leaves no NONE.X" "[ ! -f '$B/NONE.X' ]"
ok "rcc.x: server unreachable runs RCC_LOCAL (the Z's own gcc in real use)" "[ -f '$B/LOCAL.C' ]"
ok "rccd: 2 requests reached the server" "[ \$(grep -c 'files' '$OUT/rccd.log') -eq 2 ]"
sed -n 's/^/     /p' "$OUT/rccd.log"
[ -f "$B/STAT.TXT" ] && tr -d '\r' < "$B/STAT.TXT" | grep -a "pico" | sed 's/^/     /'
echo "$fails failure(s)"
exit $((fails > 0))
