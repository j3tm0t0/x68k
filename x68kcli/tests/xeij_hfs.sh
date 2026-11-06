#!/usr/bin/env bash
# XEiJ (X68000 SUPER、10MHz 68000) をホストのディレクトリから起動し、AUTOEXEC.BAT の行を流して終わるのを待つ。
#   作業ディレクトリ $OUT/boot に HUMAN.SYS・COMMAND.X (XEiJ 同梱) と FILES のファイルを置き、
#   AUTOEXEC.BAT = 引数の行 (1 引数 = 1 行。UTF-8 で書けば Shift_JIS にして書く) + 最後に DONE.TXT を作る行。
#   DONE.TXT ができるか TIMEOUT 秒たったら XEiJ を止める。結果は $OUT/boot に残る。
# 使い方: OUT=dir FILES="a.x b.txt" tests/xeij_hfs.sh 'A.X > A.TXT' ...
# FILES の名前は大文字にして置く。DIRS="dir ..." はディレクトリを中身ごと写す
# HDS=1 なら、その木を SCSI の HDS (40MB) に書いて SCSI から起動する (実機と同じ FAT の上で試す)。
#   終わったかは rb-cli で DONE.TXT を見て判断し、終わったら A:\*.TXT を $OUT/boot に取り出す
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
BOOT=(-boot="$B")
if [ -n "${HDS:-}" ]; then
  IMG=$OUT/test.hds
  python3 -I scripts/mkhds.py stage/BIN/FORMAT.X "$IMG" 40 >/dev/null
  python3 -I scripts/mkbatch.py "$B" "$IMG" > "$OUT/batch.json"
  # IPL はルートのはじめのほうしか探さないので、HUMAN.SYS をいちばん先に書く (名前順だと B2SUM.X などの後ろになり「Human.sys が見つかりません」)
  python3 -I - "$OUT/batch.json" <<'PY'
import json, sys
p = sys.argv[1]; j = json.load(open(p))
ops = j['operations']
key = lambda o: 0 if o.get('dst', '').upper() == '/HUMAN.SYS' else 1
ops.sort(key=key)
json.dump(j, open(p, 'w'))
PY
  tools/rb-cli batch --progress never "$OUT/batch.json" > /dev/null 2>&1
  python3 -I scripts/fixfs.py "$IMG" > /dev/null
  BOOT=(-boot=sc0 -si0="$IMG")
fi
done_p() {
  if [ -n "${HDS:-}" ]; then tools/rb-cli ls "$IMG" / 2>/dev/null | grep -q " DONE.TXT$"; else [ -f "$B/DONE.TXT" ]; fi
}
AG=; [ -f "${AGENTJAR:-$XEIJ/../xeijagent.jar}" ] && AG="-javaagent:${AGENTJAR:-$XEIJ/../xeijagent.jar}=$OUT/agent.cmd"
# shellcheck disable=SC2086
"$JAVA" $AG -Djava.io.tmpdir="$OUT/tmp" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER \
  "${BOOT[@]}" -flrinformed=on -usecanvas=off > "$OUT/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
ok=
for t in $(seq 1 "${TIMEOUT:-300}"); do
  done_p && { ok=1; break; }
  kill -0 $PID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij.log)"; exit 1; }
  sleep 1
done
# 時間切れのときはテキスト VRAM を $OUT/tvram.bin に取る (エラーの枠を見るため。x68k-zos の kernel/tests/tvram2txt.py で文字にする)
if [ -z "$ok" ] && [ -n "$AG" ]; then
  echo "dump $OUT/tvram.bin 0xE00000 0x80000" > "$OUT/agent.cmd"
  for t in $(seq 1 20); do [ -f "$OUT/agent.cmd" ] || break; sleep 0.5; done; sleep 1
fi
if [ -n "${HDS:-}" ]; then
  sleep 2; kill $PID 2>/dev/null || true; sleep 1
  rm -rf "$OUT/got"; tools/rb-cli get "$IMG" '/*.TXT' "$OUT/got" > /dev/null 2>&1 || true   # 既にあるファイルには書かないので別の場所へ
  cp "$OUT"/got/*.TXT "$B"/ 2>/dev/null || true
fi
[ -n "$ok" ] || { echo "時間切れ (${TIMEOUT:-300} 秒)"; exit 1; }
