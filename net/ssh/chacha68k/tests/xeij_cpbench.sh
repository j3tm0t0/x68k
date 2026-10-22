#!/usr/bin/env bash
# XEiJ (X68000 SUPER、10MHz 68000) で cpbench.x を動かし、結果 (CPBENCH.TXT) を表示する。
#   Human68k をホストのディレクトリから起動し、AUTOEXEC.BAT が cpbench.x を走らせて DONE.TXT を作るのを待つ。
#   java -jar が HeadlessException で落ちる環境 (GUI の無いセッション) では XEIJ_ITERM=1 で iTerm 経由 (open -a iTerm) で起動する。
# 使い方: tests/xeij_cpbench.sh [CPBENCH.X] [cpbench の引数...]
set -euo pipefail
cd "$(dirname "$0")"
X=${1:-cpbench.x}; shift || true
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
OUT=${CP_OUT:-${TMPDIR:-/tmp}/chacha68k}/xeij
B="$OUT/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
rm -rf "$OUT"; mkdir -p "$B" "$OUT/tmp"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp "$X" "$B/CPBENCH.X"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\n' > "$B/CONFIG.SYS"
printf 'CPBENCH.X %s CPBENCH.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n' "$*" > "$B/AUTOEXEC.BAT"
cat > "$OUT/run.command" <<CMD
#!/bin/bash
"$JAVA" -Djava.io.tmpdir="$OUT/tmp" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
  -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off > "$OUT/xeij.log" 2>&1 &
echo \$! > "$OUT/pid"
wait
CMD
chmod +x "$OUT/run.command"
if [ "${XEIJ_ITERM:-0}" = 1 ]; then open -a iTerm "$OUT/run.command"; else bash "$OUT/run.command" & fi
for t in $(seq 1 1200); do
  [ -f "$B/DONE.TXT" ] && break
  sleep 1
done
sleep 1
[ -f "$OUT/pid" ] && kill "$(cat "$OUT/pid")" 2>/dev/null || true
[ -f "$B/DONE.TXT" ] || { echo "時間切れ (ログ: $OUT/xeij.log)"; exit 1; }
cat "$B/CPBENCH.TXT"
