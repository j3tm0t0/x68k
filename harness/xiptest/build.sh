#!/bin/sh
# lock.S / lockfix.S -> lock.bin / lockfix.bin (0xF4E-0x1033 の 230 バイト) と、再配置する位置の一覧 (*.rel)
set -e
cd "$(dirname "$0")"
for n in lock lockfix; do
  m68k-xelf-as -m68000 -o $n.o $n.S
  m68k-xelf-ld -T lock.ld -o $n.elf $n.o 2>&1 | grep -v "entry symbol" || true
  m68k-xelf-objcopy -O binary -j .text $n.elf $n.full
  dd if=$n.full of=$n.bin bs=1 skip=2 2>/dev/null
  rm -f $n.full
  [ "$(wc -c < $n.bin)" -eq 230 ] || { echo "$n.bin は 230 バイトでない"; exit 1; }
  m68k-xelf-nm $n.elf | awk '$3 ~ /^R_/ {print $3, "0x" $1}' > $n.rel
  echo "$n: $(tr '\n' ' ' < $n.rel)"
done
