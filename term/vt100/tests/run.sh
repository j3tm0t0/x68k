#!/usr/bin/env bash
# vt100 エミュレータの試験一式 (Mac 上)。最後に PASS/FAIL の数を出し、FAIL があれば終了コード 1。
#   1. glyph   グリフ試験 (tests/glyphtest.py): 全角・かな・記号・ANK の VRAM が CGROM の正しい位置の字か
#              + 変異版 (dd8e297 より前の漢字アドレス計算) では落ちること (試験自体の検出力の確認)
#   2. 各フィクスチャ (tests/fixtures/*.rec) を vthost (実機と同じ描画路、VRAM はホストメモリ) に流し、
#      pyte (参照レンダラ) と文字・属性 (前景/背景/下線) を比べる。サイズは fixtures/sizes.txt (無ければ 80x24)
#   3. 分割耐性: 1 バイトずつ / 7 バイトずつ vt_write しても、まとめて流したときと画面・VRAM が同じか
#   4. 各フィクスチャの画面を PNG (768x512、16 色) に書き出す (VT_OUT、既定 ${TMPDIR:-/tmp}/vttest)
# CGROM (tests/cgrom/CGROM.DAT、tests/fetch_cgrom.sh で用意) が無ければ実フォントを使う 1 と 4 は飛ばす。
# XEiJ での計測は tests/xeij_bench.sh。
# 使い方: tests/run.sh [フィクスチャ名...]
set -uo pipefail
cd "$(dirname "$0")/.."
PY=tests/venv/bin/python
[ -x "$PY" ] || { echo "pyte の venv がない。tests/venv を作って pyte と Pillow を入れること:"; \
  echo "  python3 -m venv tests/venv && tests/venv/bin/pip install pyte pillow"; exit 1; }
PY="$PY -W ignore"
OUT=${VT_OUT:-${TMPDIR:-/tmp}/vttest}
mkdir -p "$OUT"
CG=tests/cgrom/CGROM.DAT
[ -f "$CG" ] || tests/fetch_cgrom.sh >/dev/null 2>&1 || true
CGOPT=(); [ -f "$CG" ] && CGOPT=(-g "$CG")

make -s vthost 2>&1 | grep -v 'reducing alignment' || true
npass=0; nfail=0
pass() { echo "PASS  $1"; npass=$((npass + 1)); }
fail() { echo "FAIL  $1"; nfail=$((nfail + 1)); }

# ---- 1. グリフ ----
if [ -f "$CG" ]; then
  if $PY tests/glyphtest.py ./vthost "$CG" "$OUT/glyph" > "$OUT/glyph.log" 2>&1; then pass "glyph"
  else fail "glyph"; sed 's/^/   /' "$OUT/glyph.log" | head -20; fi
  # 変異版: kanji_glyph から「16 区以降は 7 区詰める」を外したもの。グリフ試験が落ちなければ試験が甘い
  M="$OUT/mutant"; mkdir -p "$M"
  sed -e '/if (ku >= 16) ku -= 7;/d' -e 's/ || (ku >= 9 \&\& ku <= 15))/)/' vt100.c > "$M/vt100.c"
  if cmp -s vt100.c "$M/vt100.c"; then fail "glyph-mutant (vt100.c の漢字アドレス計算が変わって変異を作れない)"
  elif cc -O2 -DVT_HOSTTEST -I. -o "$M/vthost_old" vtcat.c "$M/vt100.c" vtdump.c 2>/dev/null &&
       ! $PY tests/glyphtest.py "$M/vthost_old" "$CG" "$M/out" >/dev/null 2>&1; then pass "glyph-mutant (旧アドレス計算は検出される)"
  else fail "glyph-mutant (旧アドレス計算を検出できない)"; fi
else
  echo "SKIP  glyph (CGROM が無い: tests/fetch_cgrom.sh)"
fi

# ---- 2〜4. フィクスチャ ----
size_of() {   # fixtures/sizes.txt: "名前 桁 行"
  local s
  s=$(awk -v n="$1" '$1 == n { print $2 " " $3 }' tests/fixtures/sizes.txt 2>/dev/null)
  echo "${s:-80 24}"
}
if [ $# -gt 0 ]; then recs=(); for n in "$@"; do recs+=("tests/fixtures/$n.rec"); done
else recs=(tests/fixtures/*.rec); fi
for rec in "${recs[@]}"; do
  name=$(basename "$rec" .rec)
  read -r COLS ROWS <<< "$(size_of "$name")"
  base="$OUT/$name"
  ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -s ${COLS}x${ROWS} -a "$base.attr" -v "$base.vram" "$rec" "$base.txt"
  exp=(); ref=pyte
  [ -f "tests/fixtures/$name.expect" ] && { exp=("tests/fixtures/$name.expect"); ref=expect; }
  if msg=$($PY tests/compare.py "$rec" "$COLS" "$ROWS" "$base.txt" "$base.attr" ${exp[@]+"${exp[@]}"} 2>&1); then pass "$name (${COLS}x${ROWS} vs $ref)"
  else fail "$name (${COLS}x${ROWS} vs $ref)"; echo "$msg" | head -30; fi
  ok=1
  for ch in 1 7; do
    ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -c $ch -s ${COLS}x${ROWS} -a "$base.c$ch.attr" -v "$base.c$ch.vram" "$rec" "$base.c$ch.txt"
    cmp -s "$base.txt" "$base.c$ch.txt" && cmp -s "$base.attr" "$base.c$ch.attr" && cmp -s "$base.vram" "$base.c$ch.vram" || { ok=0; echo "   -c $ch で結果が変わる"; }
    rm -f "$base.c$ch".*
  done
  [ $ok = 1 ] && pass "$name (分割 -c 1/-c 7)" || fail "$name (分割 -c 1/-c 7)"
  if [ -f "tests/fixtures/$name.reply" ]; then   # 問い合わせへの応答
    ./vthost -s ${COLS}x${ROWS} -r "$base.reply" "$rec" /dev/null
    if cmp -s "$base.reply" "tests/fixtures/$name.reply"; then pass "$name (応答)"
    else fail "$name (応答)"; echo "   mine: $(od -c "$base.reply" | head -5)"; echo "   want: $(od -c "tests/fixtures/$name.reply" | head -5)"; fi
  fi
  [ -f "$CG" ] && $PY tests/vram2png.py "$base.vram" "$base.png"
done
# ---- 5. 頑健性: ASan/UBSan 版で全フィクスチャ + 乱数で壊したシーケンス (落ちない・止まらない・配列の外を触らない) ----
if cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DVT_HOSTTEST -o "$OUT/vthost_asan" vtcat.c vt100.c vtdump.c 2>/dev/null; then
  ok=1
  for rec in "${recs[@]}"; do
    read -r COLS ROWS <<< "$(size_of "$(basename "$rec" .rec)")"
    "$OUT/vthost_asan" -c 3 -s ${COLS}x${ROWS} -r /dev/null "$rec" /dev/null 2> "$OUT/asan.log" || { ok=0; echo "   $rec"; head -5 "$OUT/asan.log"; }
  done
  [ $ok = 1 ] && pass "asan (フィクスチャ)" || fail "asan (フィクスチャ)"
  if msg=$($PY tests/fuzz.py "$OUT/vthost_asan" "$OUT/fuzz" ${VT_FUZZ:-300} 2>&1); then pass "fuzz (${VT_FUZZ:-300} 本)"
  else fail "fuzz"; echo "$msg" | tail -20; fi
else
  echo "SKIP  asan/fuzz (cc -fsanitize が使えない)"
fi
echo "---- $npass PASS, $nfail FAIL  (PNG: $OUT/*.png)"
[ $nfail = 0 ]
