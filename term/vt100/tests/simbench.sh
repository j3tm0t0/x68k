#!/usr/bin/env bash
# 68000 の命令サイクル数でスループットを見積もる (XEiJ が使えないとき・版どうしの比較用)。
#   vt100.c を VT_HOSTTEST (VRAM はメモリ上の配列) + VT_SIM_TXRAS で 68000 向けに素のバイナリにし、
#   Musashi (68000 エミュレータ、MIT) で各フィクスチャを vt_write に流したクロック数を 10MHz で割る。
#   TXRASCOPY はシミュレータが演じ、1 ラスタ = 水平 1 周期 (318 クロック) + 呼び出し 2000 クロックを足す
#   (IPLROM の _TXRASCPY はラスタごとに H-sync を待つ)。テキスト VRAM へのアクセスは 1 バスサイクル 2 クロックの
#   ウェイトを足す (XEiJ の X68000 10MHz の設定値)。メインメモリのウェイトやリフレッシュは数えない。
#   ついでに 68000 版の最終 VRAM がホスト版 (vthost -v) と同じかも確かめる (ビッグエンディアン・int 幅の違いの検出)。
# 使い方: tests/simbench.sh [-c CHUNK] [-p] [REV...]
#   REV を並べると git のその版 (term/vt100) も同じ条件で測って横に並べる (作業ツリーは "work")。
#   -p は関数ごとの所要割合 (PC サンプリング、インライン展開なし) も出す。
#   -P は通常版 (インライン展開あり) を 1 命令ずつ実行し、関数ごとのクロックの割合を正確に出す
#      (命令アドレスごとのクロックは VT_OUT/m68ksim/<名前>.pc、逆アセンブルは work.dis)。
#   -n は blit を抜いた版 (VT_PROF_NOBLIT、解析と差分走査だけ) を測る (VRAM の照合は飛ばす)。
# Musashi は VT_OUT/m68ksim/musashi に取ってくる (git clone、固定コミット)。
set -euo pipefail
cd "$(dirname "$0")/.."
CHUNK=1400; PROF=0; XCF=()
while [ $# -gt 0 ]; do case "$1" in -c) CHUNK=$2; shift 2;; -p) PROF=1; shift;; -P) PROF=2; shift;; -n) XCF=(-DVT_PROF_NOBLIT); shift;; *) break;; esac; done
OUT=${VT_OUT:-${TMPDIR:-/tmp}/vttest}/m68ksim
MUSASHI_REV=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
mkdir -p "$OUT"
CG=tests/cgrom/CGROM.DAT
[ -f "$CG" ] || tests/fetch_cgrom.sh
if [ ! -x "$OUT/sim" ] || [ tests/m68ksim/sim.c -nt "$OUT/sim" ]; then
  [ -d "$OUT/musashi" ] || git clone -q https://github.com/kstenerud/Musashi.git "$OUT/musashi"
  git -C "$OUT/musashi" checkout -q $MUSASHI_REV
  mkdir -p "$OUT/build"; cp "$OUT"/musashi/*.c "$OUT"/musashi/*.h "$OUT/build/"; cp -r "$OUT/musashi/softfloat" "$OUT/build/"
  cc -O2 -o "$OUT/build/m68kmake" "$OUT/build/m68kmake.c"
  ( cd "$OUT/build" && ./m68kmake . m68k_in.c > /dev/null )
  cc -O2 -w -I"$OUT/build" -o "$OUT/sim" tests/m68ksim/sim.c "$OUT"/build/m68kcpu.c "$OUT"/build/m68kops.c \
    "$OUT"/build/m68kdasm.c "$OUT"/build/softfloat/softfloat.c
fi
build() {   # build SRCDIR NAME [CFLAGS...]
  local src=$1 name=$2; shift 2
  m68k-xelf-gcc -O2 -m68000 -fomit-frame-pointer -finput-charset=utf-8 -fexec-charset=cp932 -ffreestanding -nostdlib \
    -DVT_HOSTTEST -DVT_SIM_TXRAS "$@" -I"$src" -Wl,-Ttext=0x1000 -Wl,-e,_start -o "$OUT/$name.elf" \
    ${XCF[@]+"${XCF[@]}"} tests/m68ksim/start.s tests/m68ksim/benchmain.c "$src/vt100.c" -lgcc
  m68k-xelf-nm "$OUT/$name.elf" > "$OUT/$name.nm"
  m68k-xelf-objcopy -O binary -j .text -j .data "$OUT/$name.elf" "$OUT/$name.bin"
}
VERS=(work)
build . work
for rev in "$@"; do
  d="$OUT/src_$rev"; rm -rf "$d"; mkdir -p "$d"
  git archive "$rev" . | tar -x -C "$d"
  # 古い版は VT_SIM_TXRAS を知らないので、TXRASCOPY の分岐をシミュレータでも通るように書き換える
  python3 -I - "$d/vt100.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p, encoding="utf-8").read()
if "VT_SIM_TXRAS" not in s:
    s = s.replace("static int _iocs_b_keyinp(void) { return 0; }",
                  "static int _iocs_b_keyinp(void) { return 0; }\nvoid _iocs_txrascpy(int d1, int d2, int d3);", 1)
    s = s.replace("#ifndef VT_HOSTTEST\n  if (dst_line < src_line", "#if 1\n  if (dst_line < src_line", 1)
open(p, "w", encoding="utf-8").write(s)
PY
  build "$d" "r_$rev"; VERS+=("r_$rev")
done
make -s vthost 2>/dev/null
LIST=${VT_BENCH_LIST:-app_claude_stream app_claude app_claude_trust app_tmux app_vimjp app_top app_lessu8 app_gitlog vim top jp_less}
printf "%-17s %7s" fixture bytes; for v in "${VERS[@]}"; do printf " %10s" "${v#r_}"; done; echo "   (bytes/s, 10MHz 68000, -c $CHUNK)"
bad=0
for n in $LIST; do
  sz=$(awk -v n="$n" '$1 == n { print $2 " " $3 }' tests/fixtures/sizes.txt); sz=${sz:-80 24}
  printf "%-17s %7d" "$n" "$(wc -c < tests/fixtures/$n.rec)"
  for v in "${VERS[@]}"; do
    dump=; [ "$v" = work ] && dump="$OUT/sim.vram"
    r=$(DUMP="$dump" "$OUT/sim" "$OUT/$v.bin" "$OUT/$v.nm" "$CG" tests/fixtures/$n.rec $CHUNK $sz)
    printf " %10s" "$(echo "$r" | awk '{print $(NF-1)}')"
  done
  ./vthost -g "$CG" -c $CHUNK -s ${sz/ /x} -v "$OUT/host.vram" tests/fixtures/$n.rec /dev/null
  if [ ${#XCF[@]} -gt 0 ] || cmp -s "$OUT/sim.vram" "$OUT/host.vram"; then echo; else echo "   VRAM differs from vthost!"; bad=1; fi
done
if [ $PROF = 2 ]; then
  m68k-xelf-objdump -d "$OUT/work.elf" > "$OUT/work.dis"
  for n in $LIST; do
    sz=$(awk -v n="$n" '$1 == n { print $2 " " $3 }' tests/fixtures/sizes.txt); sz=${sz:-80 24}
    echo "== $n"; PROF=2 PCOUT="$OUT/$n.pc" "$OUT/sim" "$OUT/work.bin" "$OUT/work.nm" "$CG" tests/fixtures/$n.rec $CHUNK $sz | head -12
  done
fi
if [ $PROF = 1 ]; then
  build . prof -fno-inline -fno-inline-small-functions -fno-ipa-sra
  for n in $LIST; do
    sz=$(awk -v n="$n" '$1 == n { print $2 " " $3 }' tests/fixtures/sizes.txt); sz=${sz:-80 24}
    echo "== $n"; PROF=1 "$OUT/sim" "$OUT/prof.bin" "$OUT/prof.nm" "$CG" tests/fixtures/$n.rec $CHUNK $sz | head -10
  done
fi
[ $bad = 0 ] && echo "68000 build: final VRAM identical to vthost for all fixtures" || { echo "68000 build: VRAM MISMATCH"; exit 1; }
