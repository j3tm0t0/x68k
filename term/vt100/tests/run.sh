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
  # 差分描画の不変条件: 64 バイトごとの vt_write の後、VRAM が論理画面を全部描き直した絵と同じか
  if msg=$(./vthost ${CGOPT[@]+"${CGOPT[@]}"} -V -c 64 -s ${COLS}x${ROWS} "$rec" /dev/null 2>&1); then pass "$name (VRAM = 論理画面)"
  else fail "$name (VRAM = 論理画面)"; echo "   $msg"; fi
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
  if msg=$(VT_CGROM=$([ -f "$CG" ] && echo "$CG") $PY tests/fuzz.py "$OUT/vthost_asan" "$OUT/fuzz" ${VT_FUZZ:-300} 2>&1); then pass "fuzz (${VT_FUZZ:-300} 本)"
  else fail "fuzz"; echo "$msg" | tail -20; fi
else
  echo "SKIP  asan/fuzz (cc -fsanitize が使えない)"
fi
# ---- 6. 仮想コンソール (-DVT_VCONS、ssh.x の Ctrl+F1..F4): 4 つの端末に混ぜて流し、裏の端末が VRAM を汚さず、
#         各端末の結果が単独で流したときと同じか (tests/vctest.c)。ASan 版で ----
if cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/vctest" tests/vctest.c vt100.c 2>/dev/null ||
   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/vctest" tests/vctest.c vt100.c 2>/dev/null; then
  F=tests/fixtures
  for set in "app_top app_vimjp app_claude_stream syn_queries" "app_tmux app_emoji app_lessu8 app_gitlog" "shell syn_altscreen syn_region jp_less"; do
    a=(); for n in $set; do a+=("$F/$n.rec"); done
    if msg=$("$OUT/vctest" ${CGOPT[@]+"${CGOPT[@]}"} "${a[@]}" 2>&1); then pass "vcons ($set)"
    else fail "vcons ($set)"; echo "$msg" | tail -10; fi
  done
else
  fail "vcons (vctest をコンパイルできない)"
fi
# ---- 7. ペイン (Z-OS の zos.x の下: _B_CONSOL が返す枠の中だけに描く)。vthost -P で枠を与え、全面で描いた VRAM を
#         枠の位置にずらしたものと同じか、枠の外を汚さないか (tests/panecmp.py)、VRAM = 論理画面 (-V) か ----
for spec in "shell 48 0 48 31" "app_vimjp 48 0 48 31" "app_lessu8 0 0 47 31" "app_emoji 24 2 40 20" "syn_region 48 0 48 31"; do
  read -r name px py pw ph <<< "$spec"
  rec=tests/fixtures/$name.rec
  [ -f "$rec" ] || { fail "pane $name (フィクスチャが無い)"; continue; }
  ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -s ${pw}x${ph} -v "$OUT/pane_full.vram" "$rec" "$OUT/pane_full.txt" > /dev/null 2>&1
  ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -P $px,$py,$pw,$ph -s 96x32 -v "$OUT/pane_pane.vram" "$rec" "$OUT/pane_pane.txt" > /dev/null 2>&1
  if ! cmp -s "$OUT/pane_full.txt" "$OUT/pane_pane.txt"; then fail "pane $name (論理画面が全面 ${pw}x${ph} と違う)"
  elif ! msg=$($PY tests/panecmp.py "$OUT/pane_full.vram" "$OUT/pane_pane.vram" $px $py $pw $ph 2>&1); then fail "pane $name"; echo "$msg" | head -8
  elif ! msg=$(./vthost ${CGOPT[@]+"${CGOPT[@]}"} -V -c 64 -P $px,$py,$pw,$ph -s 96x32 "$rec" /dev/null 2>&1); then fail "pane $name (VRAM != 論理画面)"; echo "$msg" | head -5
  else pass "pane $name ($pw x $ph @ $px,$py)"; fi
done
# ---- 8. 表の端末を何枚も並べる (vt_con_rect、zos.x のペイン): 4 枚に混ぜて流し、途中で枠を入れ替え、最後の VRAM が
#         各フィクスチャを単独で流した VRAM を枠に置いたものと同じか (tests/mptest.c、tests/mpcmp.py) ----
if cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/mptest" tests/mptest.c vt100.c 2>/dev/null ||
   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/mptest" tests/mptest.c vt100.c 2>/dev/null; then
  F=tests/fixtures
  for set in "shell:0,1,48,15 app_vimjp:48,1,48,15 jp_less:0,17,48,15 app_lessu8:48,17,48,15" "app_top:0,0,47,32 syn_region:48,0,48,32"; do
    a=(); c=()
    for spec in $set; do
      n=${spec%%:*}; r=${spec#*:}; IFS=, read -r x y w h <<< "$r"
      a+=("$F/$n.rec:$r")
      ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -s ${w}x${h} -v "$OUT/mp_$n.vram" "$F/$n.rec" /dev/null > /dev/null 2>&1
      c+=("$OUT/mp_$n.vram" "$r")
    done
    if msg=$("$OUT/mptest" ${CGOPT[@]+"${CGOPT[@]}"} -o "$OUT/mp.vram" "${a[@]}" 2>&1) &&
       msg2=$($PY tests/mpcmp.py "$OUT/mp.vram" "${c[@]}" 2>&1); then pass "multipane ($set)"
    else fail "multipane ($set)"; echo "$msg" | tail -5; echo "${msg2:-}" | tail -5; fi
  done
else
  fail "multipane (mptest をコンパイルできない)"
fi
# ---- 9. zos.x のペインの枠の変化についていく (ssh.x、-DVT_VCONS): 隠れる・別の枠で見える (tests/dyntest.c) ----
if cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/dyntest" tests/dyntest.c vt100.c 2>/dev/null ||
   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o "$OUT/dyntest" tests/dyntest.c vt100.c 2>/dev/null; then
  for n in shell app_vimjp app_top; do
    if msg=$("$OUT/dyntest" ${CGOPT[@]+"${CGOPT[@]}"} tests/fixtures/$n.rec 2>&1); then pass "pane follow $n"
    else fail "pane follow $n"; echo "$msg" | tail -5; fi
  done
else
  fail "pane follow (dyntest をコンパイルできない)"
fi
# ---- 10. 小さい字 (6x12、128x42、vt_set_small): 全面で VRAM = 論理画面 (-V)、枠を並べても各枠 = 単独 (mptest -F) ----
for n in shell app_vimjp jp_less app_top app_emoji syn_region; do
  if msg=$(./vthost ${CGOPT[@]+"${CGOPT[@]}"} -F -V -c 64 -s 128x42 tests/fixtures/$n.rec /dev/null 2>&1); then pass "small $n (128x42)"
  else fail "small $n"; echo "$msg" | head -5; fi
done
if [ -x "$OUT/mptest" ]; then
  F=tests/fixtures
  set="shell:0,1,64,20 app_vimjp:64,1,64,20 jp_less:0,22,64,20 app_lessu8:64,22,64,20"
  a=(); c=()
  for spec in $set; do
    n=${spec%%:*}; r=${spec#*:}; IFS=, read -r x y w h <<< "$r"
    a+=("$F/$n.rec:$r")
    ./vthost ${CGOPT[@]+"${CGOPT[@]}"} -F -s ${w}x${h} -v "$OUT/mps_$n.vram" "$F/$n.rec" /dev/null > /dev/null 2>&1
    c+=("$OUT/mps_$n.vram" "$r")
  done
  if msg=$("$OUT/mptest" ${CGOPT[@]+"${CGOPT[@]}"} -F -o "$OUT/mps.vram" "${a[@]}" 2>&1) &&
     msg2=$($PY tests/mpcmp.py -F "$OUT/mps.vram" "${c[@]}" 2>&1); then pass "small multipane"
  else fail "small multipane"; echo "$msg" | tail -5; echo "${msg2:-}" | tail -5; fi
fi
echo "---- $npass PASS, $nfail FAIL  (PNG: $OUT/*.png)"
[ $nfail = 0 ]
