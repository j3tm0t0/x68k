#!/usr/bin/env bash
# XEiJ (X68000 SUPER、10MHz 68000) でスループットを測る。ホスト (Mac) の bytes/s は実機と無関係なので、
# 計測版 vtbench.x (と blit を抜いた vtbnob.x) をホストファイルシステムから起動した Human68k で動かす。
#   1. 作業ディレクトリ (VT_OUT/xeij/boot) に HUMAN.SYS・COMMAND.X (XEiJ 同梱)、計測版、フィクスチャ (8.3 名) を置く
#   2. AUTOEXEC.BAT が各フィクスチャを vt_write に流して RESULT.TXT に 1 行ずつ追記し、最後に DONE.TXT を作る
#   3. XEiJ を -boot=<そのディレクトリ> -model=SUPER -config=default -saveonexit=off で起動し (ユーザ設定に触れない)、
#      DONE.TXT ができたら終了させて RESULT.TXT を表示する
# 使い方: tests/xeij_bench.sh [-c CHUNK]   (CHUNK = 1 回の vt_write のバイト数。既定 1400 ≒ TCP の 1 セグメント)
# 環境変数: XEIJ=XEiJ のディレクトリ (XEiJ.jar と data/)、VT_OUT=出力先、VT_BENCH_LIST=フィクスチャ名 (空白区切り)
set -euo pipefail
cd "$(dirname "$0")/.."
CHUNK=1400
[ "${1:-}" = "-c" ] && CHUNK=$2
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
OUT=${VT_OUT:-${TMPDIR:-/tmp}/vttest}
B="$OUT/xeij/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
[ -f "$XEIJ/XEiJ.jar" ] || { echo "XEiJ が見つからない: $XEIJ (XEIJ=... で指定)"; exit 1; }
make -s vtbench.x vtbnob.x vtcat.x vthost
rm -rf "$OUT/xeij"; mkdir -p "$B" "$OUT/xeij/tmp"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp vtbench.x "$B/VTBENCH.X"; cp vtbnob.x "$B/VTBNOB.X"; cp vtcat.x "$B/VTCAT.X"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\n' > "$B/CONFIG.SYS"
size_of() { s=$(awk -v n="$1" '$1 == n { print $2 "x" $3 }' tests/fixtures/sizes.txt); echo "${s:-80x24}"; }
LIST=${VT_BENCH_LIST:-app_claude_stream app_claude app_claude_trust app_tmux app_vimjp app_top app_lessu8 app_gitlog vim top jp_less}
{
  printf 'ECHO OFF\r\n'
  i=0
  for n in $LIST; do
    i=$((i + 1)); short=$(printf 'F%02d.REC' $i)
    cp "tests/fixtures/$n.rec" "$B/$short"
    sz=$(size_of "$n")
    printf 'VTBENCH.X -b 1 -c %s -s %s %s RESULT.TXT\r\n' "$CHUNK" "$sz" "$short"
    printf 'VTBNOB.X -b 1 -c %s -s %s %s NOBLIT.TXT\r\n' "$CHUNK" "$sz" "$short"
    echo "$short $n $sz" >> "$OUT/xeij/names.txt"
  done
  # 正しさ: 下スクロール (TXRASCOPY 降順) と \ (CGROM 0x80) を含むフィクスチャの実 VRAM を書き出し、ホスト版と比べる
  cp tests/fixtures/${VT_CHECK:-app_lessu8}.rec "$B/CHECK.REC"
  printf 'VTCAT.X -c %s -s 96x32 -v CHECK.VRM CHECK.REC CHECK.TXT\r\n' "$CHUNK"
  printf 'COPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"

"$JAVA" -Djava.io.tmpdir="$OUT/xeij/tmp" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
  -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off > "$OUT/xeij/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for t in $(seq 1 600); do
  [ -f "$B/DONE.TXT" ] && break
  kill -0 $PID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij/xeij.log)"; exit 1; }
  sleep 1
done
[ -f "$B/DONE.TXT" ] || { echo "時間切れ (10 分)"; exit 1; }
kill $PID 2>/dev/null || true
./vthost -g tests/cgrom/CGROM.DAT -c $CHUNK -s 96x32 -v "$OUT/xeij/host.vram" tests/fixtures/${VT_CHECK:-app_lessu8}.rec /dev/null
if cmp -s "$B/CHECK.VRM" "$OUT/xeij/host.vram"; then echo "VRAM check (${VT_CHECK:-app_lessu8}): XEiJ = vthost  OK"
else echo "VRAM check (${VT_CHECK:-app_lessu8}): XEiJ と vthost が違う (python3 tests/vram2png.py $B/CHECK.VRM x.png で見る)"; fi
echo "== XEiJ SUPER (10MHz 68000), vt_write $CHUNK バイトずつ"
python3 -I - "$OUT/xeij/names.txt" "$B/RESULT.TXT" "$B/NOBLIT.TXT" <<'PY'
import sys, re
names = {l.split()[0]: (l.split()[1], l.split()[2]) for l in open(sys.argv[1])}
def load(p):
    d = {}
    for l in open(p, errors="replace"):
        m = re.match(r"bench (\S+) c=\d+: (\d+) bytes in ([\d.]+) s = (\d+) bytes/s(.*)", l.strip())
        if m:
            d[m.group(1).upper()] = (int(m.group(2)), float(m.group(3)), int(m.group(4)), m.group(5))
    return d
full, nob = load(sys.argv[2]), load(sys.argv[3])
print("%-18s %6s %7s %8s %8s %6s  %s" % ("fixture", "size", "bytes", "B/s", "noblit", "blit%", "counters"))
for short, (name, sz) in names.items():
    f = full.get(short); n = nob.get(short)
    if not f:
        print("%-18s (no result)" % name); continue
    pct = 100.0 * (f[1] - n[1]) / f[1] if n and f[1] > 0 else 0
    print("%-18s %6s %7d %8d %8d %5.0f%%  %s" % (name, sz, f[0], f[2], n[2] if n else 0, pct, f[3].strip(" |")))
PY
