#!/usr/bin/env bash
# Z 側の描画時間を 68000 の命令サイクル数で見積もり、素の VT100 (vt_write) と比べる。
#   term/vt100/tests/simbench.sh の Musashi シミュレータ (VT_OUT/m68ksim/sim) を使う (無ければ先にそれを 1 回走らせる)。
#   各フィクスチャについて: (a) 記録した VT バイト列を -c CHUNK で vt_write に流す時間 (vt100 の simbench と同じ)
#   (b) zterm --replay --chunk CHUNK のフレーム列を ztd_frame に流す時間 (同じ区切りで 1 フレーム)。
# 使い方: tests/simbench.sh [-c CHUNK] [FIXTURE...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VT="$HERE/../../vt100"
CHUNK=1400
[ "${1:-}" = "-c" ] && { CHUNK=$2; shift 2; }
OUT=${VT_OUT:-${TMPDIR:-/tmp}/vttest}/m68ksim
[ -x "$OUT/sim" ] || { echo "先に term/vt100/tests/simbench.sh を (同じ VT_OUT で) 1 回走らせて sim を作る" >&2; exit 1; }
CG="$VT/tests/cgrom/CGROM.DAT"
build() {   # NAME MAIN SRC...
  local name=$1 main=$2; shift 2
  m68k-xelf-gcc -O2 -m68000 -fomit-frame-pointer -finput-charset=utf-8 -fexec-charset=cp932 -ffreestanding -nostdlib \
    -DVT_HOSTTEST -DVT_SIM_TXRAS -I"$VT" -I"$HERE/../client" -Wl,-Ttext=0x1000 -Wl,-e,_start -o "$OUT/$name.elf" \
    "$VT/tests/m68ksim/start.s" "$main" "$@" -lgcc
  m68k-xelf-nm "$OUT/$name.elf" > "$OUT/$name.nm"
  m68k-xelf-objcopy -O binary -j .text -j .data "$OUT/$name.elf" "$OUT/$name.bin"
}
build vtw "$VT/tests/m68ksim/benchmain.c" "$VT/vt100.c"
build ztw "$HERE/m68ksim/ztbench.c" "$HERE/../client/ztdraw.c"
Z="$HERE/../server/target/release/zterm"
LIST=${*:-app_claude_stream app_claude app_claude_help app_top app_tmux app_vimjp app_lessu8 app_gitlog top vim jp_less}
bad=0
printf "%-18s %7s %9s %7s %9s %6s  %s\n" fixture vtbytes "vt s" ztbytes "zt s" speedup "68k VRAM vs host"
for n in $LIST; do
  f="$VT/tests/fixtures/$n.rec"
  sz=$(awk -v n="$n" '$1 == n { print $2 " " $3 }' "$VT/tests/fixtures/sizes.txt"); sz=${sz:-80 24}
  "$Z" --replay "$f" --chunk "$CHUNK" --cols ${sz% *} --rows ${sz#* } --out "$OUT/$n.zbin" 2>/dev/null
  a=$("$OUT/sim" "$OUT/vtw.bin" "$OUT/vtw.nm" "$CG" "$f" "$CHUNK" $sz | awk '{for(i=1;i<=NF;i++) if($i=="s") print $(i-1)}')
  b=$(DUMP="$OUT/zsim.vram" "$OUT/sim" "$OUT/ztw.bin" "$OUT/ztw.nm" "$CG" "$OUT/$n.zbin" 1 $sz | awk '{for(i=1;i<=NF;i++) if($i=="s") print $(i-1)}')
  # 68000 版 (term/vt100 の asm 込み) の最終 VRAM がホスト版 ztreplay と同じか
  "$HERE/../client/ztreplay" -g "$CG" -v "$OUT/zhost.vram" "$OUT/$n.zbin" /dev/null >/dev/null
  cmp -s "$OUT/zsim.vram" "$OUT/zhost.vram" && v=same || { v="VRAM DIFFERS"; bad=1; }
  printf "%-18s %7d %9s %7d %9s %6.1fx  %s\n" "$n" "$(wc -c < "$f")" "$a" "$(wc -c < "$OUT/$n.zbin")" "$b" "$(echo "$a / $b" | bc -l)" "$v"
done
exit $bad
