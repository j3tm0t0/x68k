#!/bin/bash
# cpbench.x (Human68k) を作る。dropbear を一度 build.sh で作った後 (work/ の libtomcrypt.a を使う)。
#   build_cpbench.sh [OUT]   CP68K_ASM 無しにするなら CFLAGS_EXTRA= で
set -eu
cd "$(dirname "$0")"
D=../../dropbear/work/dropbear-2022.83; LT=$D/libtomcrypt; X=../../dropbear/x68kinc
OUT=${1:-cpbench.x}
m68k-xelf-gcc -m68000 -O2 -fomit-frame-pointer -I.. -I$LT/src/headers -I$D -DLTC_SOURCE -DDROPBEAR_SVR_PASSWORD_AUTH=0 \
  -I$X -include $X/x68k_compat.h -include $D/config.h ${CFLAGS_EXTRA--DCP68K_ASM} \
  cpbench.c ../cp68k.c ../chacha68k.s ../poly68k.s $LT/libtomcrypt.a -o "$OUT"
