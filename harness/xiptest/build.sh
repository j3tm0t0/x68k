#!/bin/sh
# lock.S -> lock.bin (0xF4E-0x1033 の 230 バイト) と、再配置する位置の一覧 (lock.rel)
set -e
cd "$(dirname "$0")"
m68k-xelf-as -m68000 -o lock.o lock.S
m68k-xelf-ld -T lock.ld -o lock.elf lock.o 2>&1 | grep -v "entry symbol" || true
m68k-xelf-objcopy -O binary -j .text lock.elf lock.full
dd if=lock.full of=lock.bin bs=1 skip=2 2>/dev/null
rm -f lock.full
[ "$(wc -c < lock.bin)" -eq 230 ] || { echo "lock.bin は 230 バイトでない"; exit 1; }
m68k-xelf-nm lock.elf | awk '$3 ~ /^R_/ {print $3, "0x" $1}' > lock.rel
cat lock.rel
