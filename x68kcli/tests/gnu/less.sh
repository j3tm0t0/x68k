#!/usr/bin/env bash
# less を XEiJ でキーを打って試す。Human68k の CON (COMMAND.X から) と、zos.x のペイン (zosh から) の 2 通り。
# キーの台本・テキスト VRAM の読み取り・zos.x は x68k-zos のものを使う (ZOS=x68k-zos の置き場、既定は隣)。
#   画面 1: 1 ページ目 (Shift_JIS の行が字のまま出る、下に名前)
#   画面 2: スペースと ↓ 2 回で 30 行 + 2 行進む
#   画面 3: ↑ で 1 行戻る (zos.x のペインのみ)。/150 で 150 行目へ (CON のみ)
#   Ctrl+C では終わらない (BREAK の確かめを切っている)。q で終わり、zosh のプロンプトに戻る (zos.x のペイン)
set -u
cd "$(dirname "$0")"
X=$(cd ../.. && pwd)
ZOS=${ZOS:-$X/../../x68k-zos}
K=$ZOS/kernel/tests
[ -f "$K/xeij_run.sh" ] || { echo "x68k-zos が無い: $ZOS (ZOS=で指定)"; exit 1; }
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-less}; rm -rf "$OUT"; mkdir -p "$OUT/in"
{ printf '日本語の行\r\n'; seq 1 200 | sed 's/$/\r/'; } | iconv -f UTF-8 -t CP932 > "$OUT/in/N.TXT"
scr() { python3 -I "$K/tvram2txt.py" "$1" 2>/dev/null | grep -v '^[[:space:]]*$'; }
ok=1
want() { if echo "$2" | grep -q -E "$3"; then echo "ok   $1"; else echo "FAIL $1 (/$3/)"; echo "$2" | head -5; ok=0; fi; }

# 1. Human68k の CON
ZOS_OUT=$OUT/con ZOS_TIMEOUT=90 FILES="$X/build/x/gnu/less.x $OUT/in/N.TXT" \
ZOS_KEYS="sleep 20;dump c1.bin 0xE00000 0x80000;key space;sleep 3;key down;sleep 1;key down;sleep 4;dump c2.bin 0xE00000 0x80000;type /150\n;sleep 4;dump c3.bin 0xE00000 0x80000;type q" \
  "$K/xeij_run.sh" 'LESS.X N.TXT' > "$OUT/con.log" 2>&1
s1=$(scr "$OUT/con/c1.bin"); s2=$(scr "$OUT/con/c2.bin"); s3=$(scr "$OUT/con/c3.bin")
want "CON: 1 ページ目の先頭は 2 バイト文字の行、次が 1" "$(echo "$s1" | head -2 | tr '\n' ' ')" '^#+ 1 $'
want "CON: 下に名前" "$s1" '^N\.TXT'
want "CON: スペースと ↓↓ で 32 から" "$(echo "$s2" | head -1)" '^32$'
want "CON: /150 で 150 行目へ" "$(echo "$s3" | head -1)" '^150$'
[ -f "$OUT/con/boot/DONE.TXT" ] && echo "ok   CON: q で終わる" || { echo "FAIL CON: q で終わらない"; ok=0; }

# 2. zos.x のペイン (zosh)
make -s -C "$ZOS/kernel" zos.x >/dev/null 2>&1; make -s -C "$ZOS/shell/src" >/dev/null 2>&1
ZOS_OUT=$OUT/zos ZOS_VRAM=1 ZOS_CONFIG='ENVSET = 8192' ZOS_TIMEOUT=150 \
FILES="$ZOS/kernel/zos.x $ZOS/shell/src/zosh.x $X/build/x/gnu/less.x $OUT/in/N.TXT" \
ZOS_KEYS='waittext A: 60;sleep 3;type less.x N.TXT\n;sleep 20;dump z1.bin 0xE00000 0x80000;key down;sleep 1;key down;sleep 1;key space;sleep 4;dump z2.bin 0xE00000 0x80000;key up;sleep 3;dump z3.bin 0xE00000 0x80000;key ctrl c;sleep 3;dump z5.bin 0xE00000 0x80000;type q;sleep 3;type echo back\n;sleep 3;dump z4.bin 0xE00000 0x80000;type exit\n' \
  "$K/xeij_run.sh" 'ZOS.X -x -o SNAP.TXT A:\ZOSH.X' > "$OUT/zos.log" 2>&1
z1=$(scr "$OUT/zos/z1.bin" | sed 1d); z2=$(scr "$OUT/zos/z2.bin" | sed 1d); z3=$(scr "$OUT/zos/z3.bin" | sed 1d); z4=$(scr "$OUT/zos/z4.bin")
want "zos: 1 ページ目の先頭は 2 バイト文字の行、次が 1" "$(echo "$z1" | head -2 | tr '\n' ' ')" '^#+ 1 $'
want "zos: ↓↓ とスペースで 32 から" "$(echo "$z2" | head -1)" '^32$'
want "zos: ↑ で 31 から" "$(echo "$z3" | head -1)" '^31$'
want "zos: Ctrl+C で less は終わらない (31 から、下に :)" "$(scr "$OUT/zos/z5.bin" | sed 1d | head -1) $(scr "$OUT/zos/z5.bin" | tail -1)" '^31 :'
want "zos: q の後に zosh に戻る" "$z4" '^back'
[ $ok = 1 ] && echo PASS || exit 1
