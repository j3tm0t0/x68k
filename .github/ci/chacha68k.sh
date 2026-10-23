#!/usr/bin/env bash
# CI: net/ssh/chacha68k の試験 (net/ssh/chacha68k/README.md の手順のうち、XEiJ を使わないもの)。
#   Musashi (68000 エミュレータ、MIT) を固定コミットで取ってきて tests/csim を作り、asm 版を csim で、C 版をホストで
#   python cryptography (tests/ref.py) と比べる。dbtest は FAST=1 ./build.sh 済みの dropbear の chachapoly.o を使う。
# 要るもの: m68k-xelf-gcc (elf2x68k)、cryptography の入った python (PY=)、net/ssh/dropbear の build.sh 済みの work/
set -euo pipefail
cd "$(dirname "$0")/../../net/ssh/chacha68k/tests"
PY=${PY:-python3}
export S=${S:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/chacha68k}; mkdir -p "$S"
MUSASHI_REV=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd   # term/vt100/tests/simbench.sh と同じ
M=$S/musashi
if [ ! -f "$M/build/m68kops.c" ]; then
  rm -rf "$M"; git clone -q https://github.com/kstenerud/Musashi.git "$M/src"
  git -C "$M/src" checkout -q $MUSASHI_REV
  mkdir -p "$M/build"; cp "$M"/src/*.c "$M"/src/*.h "$M/build/"; cp -r "$M/src/softfloat" "$M/build/"
  cc -O2 -o "$M/build/m68kmake" "$M/build/m68kmake.c"
  ( cd "$M/build" && ./m68kmake . m68k_in.c > /dev/null )
fi
cc -O2 -w -I"$M/build" -o "$S/csim" csim.c "$M"/build/m68kcpu.c "$M"/build/m68kops.c "$M"/build/m68kdasm.c "$M"/build/softfloat/softfloat.c
D=../../dropbear/work/dropbear-2022.83
[ -f "$D/chachapoly.o" ] || { echo "chacha68k: $D/chachapoly.o が無い (先に net/ssh/dropbear で ./fetch.sh && FAST=1 ./build.sh)"; exit 1; }
echo "== csim (asm): cp68k を cryptography と差分 =="
./mk.sh cpx -DCP68K_ASM benchcp.c ../cp68k.c ../poly68k.s ../chacha68k.s
"$PY" -I difftest.py 200 "$S/csim" "$S/cpx.bin" "$S/cpx.nm"
echo "== csim (asm): dropbear の chachapoly.o (dbtest) =="
./mk.sh dbt -DCP68K_ASM dbtest.c "$D/chachapoly.o" ../cp68k.c ../chacha68k.s ../poly68k.s
"$PY" -I difftest.py 100 "$S/csim" "$S/dbt.bin" "$S/dbt.nm"
echo "== RFC 8439 (csim の asm 版とホストの C 版) =="
./mk.sh rfc -DCP68K_ASM -DCSIM rfc8439.c ../cp68k.c ../chacha68k.s ../poly68k.s
"$S/csim" "$S/rfc.bin" "$S/rfc.nm"
cc -O2 -I.. -o "$S/rfc" rfc8439.c ../cp68k.c
"$S/rfc"
echo "== ホスト (C 版) を cryptography と差分 =="
cc -O2 -I.. -o "$S/hosttool" hosttool.c ../cp68k.c
"$PY" -I difftest.py 300 sh -c 'exec "$0" "$1" > "$2"' "$S/hosttool"
echo "ALL CHACHA68K TESTS PASSED"
