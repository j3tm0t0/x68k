#!/usr/bin/env bash
# XEiJ (X68000 SUPER、10MHz 68000) をホストのディレクトリから起動し、AUTOEXEC.BAT の行を流して終わるのを待つ。
#   作業ディレクトリ $OUT/boot に HUMAN.SYS・COMMAND.X (XEiJ 同梱) と FILES のファイルを置き、
#   AUTOEXEC.BAT = 引数の行 (1 引数 = 1 行。UTF-8 で書けば Shift_JIS にして書く) + 最後に DONE.TXT を作る行。
#   DONE.TXT ができるか TIMEOUT 秒たったら XEiJ を止める。結果は $OUT/boot に残る。
# 使い方: OUT=dir FILES="a.x b.txt" tests/xeij_hfs.sh 'A.X > A.TXT' ...
# FILES の名前は大文字にして置く。DIRS="dir ..." はディレクトリを中身ごと写す
# (x68k-zos の kernel/tests/xeij_run.sh と同じ形。ユーザーの XEiJ の設定には触れない: -config=default -saveonexit=off)
set -euo pipefail
cd "$(dirname "$0")/.."
XEIJ=${XEIJ:-$PWD/tools/xeij}
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-test}
B=$OUT/boot
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
[ -f "$XEIJ/XEiJ.jar" ] || { echo "XEiJ が見つからない: $XEIJ (make tools)"; exit 1; }
rm -rf "$B" "$OUT/tmp"; mkdir -p "$B/TMP" "$OUT/tmp"   # A:\TMP は zosh のパイプの一時ファイルの置き場
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
for f in ${FILES:-}; do cp "$f" "$B/$(basename "$f" | tr a-z A-Z)"; done
for d in ${DIRS:-}; do cp -R "$d" "$B/"; done
printf 'FILES = 40\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nENVSET = 4096\r\nSHELL = A:\\COMMAND.X\r\n%s' "${CONFIG:-}" > "$B/CONFIG.SYS"
{
  printf 'ECHO OFF\r\nPATH A:\\\r\n'
  for l in "$@"; do printf '%s\r\n' "$l" | iconv -f UTF-8 -t CP932; done
  printf 'COPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"
"$JAVA" -Djava.io.tmpdir="$OUT/tmp" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER \
  -boot="$B" -flrinformed=on -usecanvas=off > "$OUT/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for t in $(seq 1 "${TIMEOUT:-300}"); do
  [ -f "$B/DONE.TXT" ] && break
  kill -0 $PID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij.log)"; exit 1; }
  sleep 1
done
[ -f "$B/DONE.TXT" ] || { echo "時間切れ (${TIMEOUT:-300} 秒)"; exit 1; }
