#!/bin/bash
# zterm サーバの画面変換が term/vt100 (vt100.c) と同じ結果になるかを、記録済みフィクスチャで確かめる。
# 各 .rec をサーバの --replay と vthost に食わせ、最終画面の文字と属性 (fg,bg,flags) を比べる。
# さらにサーバが出したフレーム列を Z 側の描画 (ztdraw.c、ztreplay -V) に通した画面がサーバの画面と同じか確かめる。
#   usage: tests/replay.sh            (要: cargo build --release、term/vt100 の make vthost、client の make ztreplay ztpredtest ztvctest)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
FX="$HERE/../../vt100/tests/fixtures"
Z="$HERE/../server/target/release/zterm"
VTH="$HERE/../../vt100/vthost"
ZR="$HERE/../client/ztreplay"
T=$(mktemp -d)
pass=0; fail=0; known=0
# vt100 crate が扱わない端の機能 (?1047、DECAWM off、TBC、IL 後のカーソル桁、ZWJ 絵文字の幅) を使う合成試験
KNOWN="syn_altscreen syn_edge syn_lines syn_queries syn_region"
for f in "$FX"/*.rec; do
  b=$(basename "$f" .rec)
  sz=$(awk -v n="$b" '$1==n{print $2"x"$3}' "$FX/sizes.txt"); [ -z "$sz" ] && sz=80x24
  c=${sz%x*}; r=${sz#*x}
  "$VTH" -s "$sz" -a "$T/$b.vta" "$f" "$T/$b.vt" >/dev/null 2>&1
  "$Z" --replay "$f" --cols "$c" --rows "$r" --dump "$T/$b.zt" --out "$T/$b.zbin" 2>/dev/null
  ok=1; vt=1
  cmp -s "$T/$b.vt" "$T/$b.zt" || vt=0
  cmp -s "$T/$b.vta" "$T/$b.zt.attr" || vt=0
  # 往復: いろいろな刻みでフレームを作り (刻みが細かいほどフレーム・スクロール・消去が増える)、Z 側で描いた画面が同じか
  for ch in 1400 64 7; do
    "$Z" --replay "$f" --cols "$c" --rows "$r" --chunk $ch --out "$T/$b.$ch.zbin" 2>/dev/null
    "$ZR" -V "$T/$b.$ch.zbin" "$T/$b.zr" >/dev/null 2>&1 || { ok=0; echo "  ztreplay -V failed (chunk $ch)"; }
    cmp -s "$T/$b.zt" "$T/$b.zr" || { ok=0; echo "  round trip differs (chunk $ch)"; }
  done
  if [ $ok = 1 ] && [ $vt = 1 ]; then pass=$((pass+1)); echo "PASS $b"
  elif [ $ok = 1 ] && echo " $KNOWN " | grep -q " $b "; then known=$((known+1)); echo "KNOWN $b (往復は一致。vt100.c との違いは vt100 crate の制限)"
  else fail=$((fail+1)); echo "FAIL $b (vt100.c と一致 $vt / 往復 $ok)"; fi
done
# 仮想コンソール (ssh.x -Z の Ctrl+F1..F4): フレーム列を 4 本 / 3 本のチャネルに混ぜて流す (client/ztvctest.c)
VC="$HERE/../client/ztvctest"
CG="$HERE/../../vt100/tests/cgrom/CGROM.DAT"; CGO=(); [ -f "$CG" ] && CGO=(-g "$CG")
for set in "app_top app_vimjp app_claude_stream app_tmux" "app_lessu8 app_gitlog app_emoji"; do
  a=(); for b in $set; do "$Z" --replay "$FX/$b.rec" --cols 96 --rows 32 --chunk 7 --out "$T/$b.vc.zbin" 2>/dev/null; a+=("$T/$b.vc.zbin"); done
  if msg=$("$VC" ${CGO[@]+"${CGO[@]}"} "${a[@]}" 2>&1); then pass=$((pass+1)); echo "PASS ztvctest ($set)"
  else fail=$((fail+1)); echo "FAIL ztvctest ($set)"; echo "$msg" | tail -5; fi
done
rm -rf "$T"
# 予測エコーの筋書き試験
if "$HERE/../client/ztpredtest" > /dev/null; then pass=$((pass+1)); echo "PASS ztpredtest (予測エコー)"; else fail=$((fail+1)); echo "FAIL ztpredtest"; "$HERE/../client/ztpredtest" | grep -A1 FAIL; fi
echo "pass $pass / known $known / fail $fail"
[ $fail = 0 ]
