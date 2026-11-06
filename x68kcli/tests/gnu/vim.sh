#!/usr/bin/env bash
# vim (vi) を XEiJ でキーを打って試す。Human68k の CON (COMMAND.X から) と、zos.x のペイン (zosh から) の 2 通り。
# 入力は Shift_JIS の行と CR LF の行。↓ で 2 行目、A で行末に XYZ を足し、↓ と x で 3 行目の最後の字を消し、
# ↑↑ と 0 → で 1 行目の 2 字目 (2 バイト文字) に動いて x で消す。:wq のあと、ファイルの中身を比べる
# (CR LF のまま、2 バイト文字が割れていないこと)。キーの台本と zos.x は x68k-zos のもの (ZOS=置き場)
set -u
cd "$(dirname "$0")"
X=$(cd ../.. && pwd)
ZOS=${ZOS:-$X/../../x68k-zos}
K=$ZOS/kernel/tests
[ -f "$K/xeij_run.sh" ] || { echo "x68k-zos が無い: $ZOS (ZOS=で指定)"; exit 1; }
VIM=${VIM:-$X/stage/USR/BIN/vim.x}; VIMRC=${VIMRC_SRC:-$X/templates/vimrc}
[ -f "$VIM" ] || { echo "vim.x が無い: $VIM (scripts/stage.sh を先に)"; exit 1; }
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-vim}; rm -rf "$OUT"; mkdir -p "$OUT/in"
printf '日本語の行\r\nabc\r\ndef\r\n' | iconv -f UTF-8 -t CP932 > "$OUT/in/T.TXT"
printf '日語の行\r\nabcXYZ\r\nde\r\n' | iconv -f UTF-8 -t CP932 > "$OUT/want.txt"
python3 -I "$X/scripts/totext.py" --crlf "$VIMRC" "$OUT/in/VIMRC" 2>/dev/null || cp "$VIMRC" "$OUT/in/VIMRC"
EDIT='key down;sleep 1;type A;sleep 1;type XYZ;key esc;sleep 1;key down;sleep 1;type $;type x;sleep 1;key up;sleep 1;key up;sleep 1;type 0;key right;sleep 1;type x;sleep 1'
ok=1
check() {   # check 名前 ファイル
  if cmp -s "$2" "$OUT/want.txt"; then echo "ok   $1: 中身 (CR LF、2 バイト文字)"; else echo "FAIL $1: 中身"; od -c "$2" | head -4; ok=0; fi
}
# 1. Human68k の CON (TERM=vt100 でも、vimrc の set term=builtin_x68k で矢印が届くこと)
ZOS_OUT=$OUT/con ZOS_TIMEOUT=90 FILES="$VIM $OUT/in/VIMRC $OUT/in/T.TXT" \
ZOS_KEYS="sleep 15;$EDIT;type :wq\n;sleep 3" "$K/xeij_run.sh" 'SET TERM=vt100' 'VIM.X T.TXT' > "$OUT/con.log" 2>&1
check CON "$OUT/con/boot/T.TXT"
# 2. zos.x のペイン (zosh)
make -s -C "$ZOS/kernel" zos.x >/dev/null 2>&1; make -s -C "$ZOS/shell/src" >/dev/null 2>&1
ZOS_OUT=$OUT/zos ZOS_CONFIG='ENVSET = 8192' ZOS_TIMEOUT=150 \
FILES="$ZOS/kernel/zos.x $ZOS/shell/src/zosh.x $VIM $OUT/in/VIMRC $OUT/in/T.TXT" \
ZOS_KEYS="waittext A: 60;sleep 3;type vim.x T.TXT\n;sleep 15;$EDIT;type :wq\n;sleep 3;type exit\n" \
  "$K/xeij_run.sh" 'ZOS.X -x -o SNAP.TXT A:\ZOSH.X' > "$OUT/zos.log" 2>&1
check zos "$OUT/zos/boot/T.TXT"
[ $ok = 1 ] && echo PASS || exit 1
