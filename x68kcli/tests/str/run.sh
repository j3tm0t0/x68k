#!/bin/bash
# src/xshim/x68str.S の試験。Musashi (68000 の模擬) の上で strtest.c を動かし、素直な C の版と比べる。
# usage: tests/str/run.sh          試験 (OK / NG)
#        BENCH=長さ tests/str/run.sh  ここの版と newlib の版の 1 回あたりのクロック数 (区間 1-12)
# Musashi は $OUT/musashi に取ってくる (term/vt100/tests/simbench.sh と同じ固定コミット)。模擬器の本体は net/ssh/chacha68k/tests/csim.c
set -eu
cd "$(dirname "$0")"
R=$(cd ../../.. && pwd)
OUT=${STR_OUT:-${TMPDIR:-/tmp}/x68kcli-str}
MUSASHI_REV=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
mkdir -p "$OUT"
if [ ! -x "$OUT/csim" ]; then
  [ -d "$OUT/musashi" ] || git clone -q https://github.com/kstenerud/Musashi.git "$OUT/musashi"
  git -C "$OUT/musashi" checkout -q $MUSASHI_REV
  mkdir -p "$OUT/build"; cp "$OUT"/musashi/*.c "$OUT"/musashi/*.h "$OUT/build/"; cp -r "$OUT/musashi/softfloat" "$OUT/build/"
  cc -O2 -o "$OUT/build/m68kmake" "$OUT/build/m68kmake.c"
  ( cd "$OUT/build" && ./m68kmake . m68k_in.c > /dev/null )
  cc -O2 -w -I"$OUT/build" -o "$OUT/csim" "$R/net/ssh/chacha68k/tests/csim.c" "$OUT"/build/m68kcpu.c "$OUT"/build/m68kops.c \
    "$OUT"/build/m68kdasm.c "$OUT"/build/softfloat/softfloat.c -lm
fi
EXTRA=()
if [ -n "${BENCH:-}" ]; then
  # newlib の版を nl_ を付けて取り込む
  L=$(m68k-xelf-gcc -print-file-name=libc.a)
  ( cd "$OUT" && for f in memcpy memset memcmp memchr strlen strcmp; do
      m68k-xelf-ar x "$L" libc_a-$f.o && m68k-xelf-objcopy --redefine-sym $f=nl_$f libc_a-$f.o nl_$f.o; done )
  EXTRA=("$OUT"/nl_*.o -DBENCH="$BENCH")
fi
m68k-xelf-gcc -O2 -m68000 -fomit-frame-pointer -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -nostdlib \
  -Wl,-Ttext=0x1000 -Wl,-e,_start -o "$OUT/strtest.elf" start.s strtest.c regsave.s ../../src/xshim/x68str.S ${EXTRA[@]+"${EXTRA[@]}"} -lgcc
m68k-xelf-nm "$OUT/strtest.elf" > "$OUT/strtest.nm"
m68k-xelf-objcopy -O binary -j .text -j .data "$OUT/strtest.elf" "$OUT/strtest.bin"
rm -f "$OUT/result.txt"   # 模擬器が途中で止まったとき、前の結果で通らないように
"$OUT/csim" "$OUT/strtest.bin" "$OUT/strtest.nm" /dev/null "$OUT/result.txt" > "$OUT/csim.out" || { cat "$OUT/csim.out"; echo "csim が失敗した"; exit 1; }
grep -v "^total" "$OUT/csim.out" || true
cat "$OUT/result.txt"
[ -n "${BENCH:-}" ] || grep -q "^OK " "$OUT/result.txt"
