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
# VIM9=1 なら vim 9 (build/x/gnu/vim9.x) を vim.x の名前で試す。既定は /usr/share/vim/vim92/defaults.vim (ZOS_STAGE で A: に写す)
if [ -n "${VIM9:-}" ]; then VIM=$X/build/x/gnu/vim9.x; fi
[ -f "$VIM" ] || { echo "vim.x が無い: $VIM (scripts/stage.sh を先に)"; exit 1; }
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-vim}; rm -rf "$OUT"; mkdir -p "$OUT/in"
printf '日本語の行\r\nabc\r\ndef\r\n' | iconv -f UTF-8 -t CP932 > "$OUT/in/T.TXT"
printf '日語の行\r\nabcXYZ\r\nde\r\n' | iconv -f UTF-8 -t CP932 > "$OUT/want.txt"
python3 -I "$X/scripts/totext.py" --crlf "$VIMRC" "$OUT/in/VIMRC" 2>/dev/null || cp "$VIMRC" "$OUT/in/VIMRC"
cp "$VIM" "$OUT/in/VIM.X"; VIM=$OUT/in/VIM.X
STG=; if [ -n "${VIM9:-}" ]; then
  STG=$OUT/stage; mkdir -p "$STG/USR/SHARE/VIM/VIM92"; cp "$X/templates/vim92/defaults.vim" "$STG/USR/SHARE/VIM/VIM92/defaults.vim"
fi
WAIT=15; [ -n "${VIM9:-}" ] && WAIT=30   # vim 9 は起動に 5 秒以上かかる。画面にファイルの 3 行目 (def) が出るまで、この 4 倍まで待つ
EDIT='key down;sleep 1;type A;sleep 1;type XYZ;key esc;sleep 1;key down;sleep 1;type $;type x;sleep 1;key up;sleep 1;key up;sleep 1;type 0;key right;sleep 1;type x;sleep 1'
ok=1
check() {   # check 名前 ファイル
  if cmp -s "$2" "$OUT/want.txt"; then echo "ok   $1: 中身 (CR LF、2 バイト文字)"; else echo "FAIL $1: 中身"; od -c "$2" | head -4; ok=0; fi
}
# 1. Human68k の CON (TERM=vt100 でも、vimrc の set term=builtin_x68k で矢印が届くこと)
ZOS_OUT=$OUT/con ZOS_VRAM=1 ZOS_TIMEOUT=120 ZOS_STAGE=$STG FILES="$VIM $OUT/in/VIMRC $OUT/in/T.TXT" \
ZOS_KEYS="waittext def $((WAIT * 4));sleep 2;$EDIT;type :wq\n;sleep 3" "$K/xeij_run.sh" 'SET TERM=vt100' 'VIM.X T.TXT' > "$OUT/con.log" 2>&1
check CON "$OUT/con/boot/T.TXT"
# 終わったあと vim の画面 (空の行の ~) が残っていないこと (vimrc の t_te で画面を消す)
if [ -f "$OUT/con/tvram.bin" ] && python3 -I "$K/tvram2txt.py" "$OUT/con/tvram.bin" 2>/dev/null | grep -q '^~ *$'; then
  echo "FAIL CON: 終わったあとに vim の画面が残っている"; python3 -I "$K/tvram2txt.py" "$OUT/con/tvram.bin" | grep -v '^[[:space:]]*$' | head -8; ok=0
elif [ -f "$OUT/con/tvram.bin" ]; then echo "ok   CON: 終わったあとに vim の画面が残らない"
else echo "FAIL CON: テキスト VRAM が取れない"; ok=0; fi
# 2. zos.x のペイン (zosh)
make -s -C "$ZOS/kernel" zos.x >/dev/null 2>&1; make -s -C "$ZOS/shell/src" >/dev/null 2>&1
ZOS_OUT=$OUT/zos ZOS_STAGE=$STG ZOS_CONFIG='ENVSET = 8192' ZOS_TIMEOUT=200 \
FILES="$ZOS/kernel/zos.x $ZOS/shell/src/zosh.x $VIM $OUT/in/VIMRC $OUT/in/T.TXT" \
ZOS_KEYS="waittext A: 60;sleep 3;type vim.x T.TXT\n;waittext def $((WAIT * 4));sleep 12;dump zc.bin 0xE00000 0x80000;$EDIT;type :wq\n;sleep 3;dump zv.bin 0xE00000 0x80000;type exit\n" \
  "$K/xeij_run.sh" 'ZOS.X -x -o SNAP.TXT A:\ZOSH.X' > "$OUT/zos.log" 2>&1
check zos "$OUT/zos/boot/T.TXT"
# キーを待っている間に CPU を使い切らない (zos.x の上の行の CPU xx%。vim 3.0 は scripts/vim3wait.py で直したもの)
cpu=$(python3 -I "$K/tvram2txt.py" "$OUT/zos/zc.bin" 2>/dev/null | head -1 | sed -n 's/.*CPU *\([0-9]*\)%.*/\1/p')
if [ -n "$cpu" ] && [ "$cpu" -lt 50 ]; then echo "ok   zos: キーを待つ間の CPU ${cpu}%"; else echo "FAIL zos: キーを待つ間の CPU ${cpu:-?}%"; ok=0; fi
# zos.x のペインは別の画面 (CSI ? 1049 h / l) を持つので、vim 9 は終わったあと vim の前の画面 (vim.x T.TXT と打った行) に戻る。
# vim 3.0 は t_ti を出さない (vimrc を読む前に端末を準備する) ので、t_te で画面を消すだけ。どちらも vim の画面 (~) は残らない
zv=$(python3 -I "$K/tvram2txt.py" "$OUT/zos/zv.bin" 2>/dev/null)
want_cmd=0; [ -n "${VIM9:-}" ] && want_cmd=1
if [ "$(echo "$zv" | grep -c 'vim\.x T\.TXT') $(echo "$zv" | grep -c '^~ *$')" = "$want_cmd 0" ]; then
  [ -n "${VIM9:-}" ] && echo "ok   zos: 終わったあと vim の前の画面に戻る" || echo "ok   zos: 終わったあと vim の画面が残らない"
else echo "FAIL zos: 終わったあとの画面"; echo "$zv" | grep -v '^[[:space:]]*$' | head -8; ok=0; fi
[ $ok = 1 ] && echo PASS || exit 1
