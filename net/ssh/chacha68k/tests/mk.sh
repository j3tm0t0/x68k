#!/bin/bash
# csim 用の素のバイナリを作る: mk.sh NAME [CFLAGS / 追加ソース...]  -> $S/NAME.bin, NAME.nm, NAME.elf
set -eu
cd "$(dirname "$0")"
S=${S:-${TMPDIR:-/tmp}/chacha68k}; mkdir -p "$S"
D=../../dropbear/work/dropbear-2022.83; LT=$D/libtomcrypt
name=$1; shift
m68k-xelf-gcc -O2 -m68000 -fomit-frame-pointer -ffreestanding -nostdlib -I.. -I$LT/src/headers -I$D -DLTC_SOURCE \
  -DDROPBEAR_SVR_PASSWORD_AUTH=0 -I../../dropbear/x68kinc -include ../../dropbear/x68kinc/x68k_compat.h -include $D/config.h \
  -Wl,-Ttext=0x1000 -Wl,-e,_start -o "$S/$name.elf" start.s "$@" baremetal.c $LT/libtomcrypt.a -lgcc
m68k-xelf-nm "$S/$name.elf" > "$S/$name.nm"
m68k-xelf-objcopy -O binary -j .text -j .data "$S/$name.elf" "$S/$name.bin"
