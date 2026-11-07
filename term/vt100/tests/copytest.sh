#!/usr/bin/env bash
# vram_move_lines の CPU で写す経路 (枠の中のスクロール) で、asm の内側の転送が C のループと同じ VRAM を作るか (Musashi)。
#   桁数 1〜96 (ロング数 1〜24) x ライン数 1〜32 の組ごとに、乱数で埋めた VRAM を 5 ライン上へ写したあとのハッシュを、
#   asm の版と C の版 (-DVT_NO_ASM) で比べる。全部の組で同じなら PASS
# Musashi は simbench.sh と同じ所 (VT_OUT/m68ksim/musashi、固定コミット) に取ってくる
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${VT_OUT:-${TMPDIR:-/tmp}/vttest}/m68ksim
MUSASHI_REV=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
mkdir -p "$OUT"
if [ ! -d "$OUT/build" ]; then
  [ -d "$OUT/musashi" ] || git clone -q https://github.com/kstenerud/Musashi.git "$OUT/musashi"
  git -C "$OUT/musashi" checkout -q $MUSASHI_REV
  mkdir -p "$OUT/build"; cp "$OUT"/musashi/*.c "$OUT"/musashi/*.h "$OUT/build/"; cp -r "$OUT/musashi/softfloat" "$OUT/build/"
  cc -O2 -o "$OUT/build/m68kmake" "$OUT/build/m68kmake.c"
  ( cd "$OUT/build" && ./m68kmake . m68k_in.c > /dev/null )
fi
cc -O2 -w -I"$OUT/build" -o "$OUT/copysim" tests/m68ksim/copysim.c "$OUT"/build/m68kcpu.c "$OUT"/build/m68kops.c \
  "$OUT"/build/m68kdasm.c "$OUT"/build/softfloat/softfloat.c -lm   # Linux は -lm が要る (Musashi の FPU の sin・cos)
build() {   # build NAME [CFLAGS...]
  local name=$1; shift
  m68k-xelf-gcc -O2 -m68000 -fomit-frame-pointer -finput-charset=utf-8 -fexec-charset=cp932 -ffreestanding -nostdlib \
    -DVT_HOSTTEST -DVT_SIM_COPYTEST "$@" -I. -Wl,-Ttext=0x1000 -Wl,-e,_start -o "$OUT/$name.elf" \
    tests/m68ksim/start.s tests/m68ksim/copymain.c vt100.c -lgcc
  m68k-xelf-objcopy -O binary -j .text -j .data "$OUT/$name.elf" "$OUT/$name.bin"
}
build copy_asm
build copy_c -DVT_NO_ASM
N=$((96 * 32 + 1))
"$OUT/copysim" "$OUT/copy_asm.bin" $N "$OUT/copy_asm.res"
"$OUT/copysim" "$OUT/copy_c.bin" $N "$OUT/copy_c.res"
# 終わりの印 (0xdeadbeef) まで書けていること (途中で止まっていない)
tail -c 4 "$OUT/copy_asm.res" | od -An -tx1 | tr -d ' \n' | grep -q deadbeef || { echo "FAIL asm の版が最後まで回っていない"; exit 1; }
if cmp -s "$OUT/copy_asm.res" "$OUT/copy_c.res"; then
  echo "PASS asm と C のループで VRAM が同じ (桁数 1〜96 x ライン数 1〜32)"
else
  echo "FAIL asm と C のループで VRAM が違う"; cmp "$OUT/copy_asm.res" "$OUT/copy_c.res" | head -3; exit 1
fi
