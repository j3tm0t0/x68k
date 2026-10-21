#!/bin/bash
# dropbear dbclient を elf2x68k でクロスビルドして ssh.x を作る。
#   1. fetch.sh で取得・展開した work/dropbear-VER に
#   2. localoptions.h と x68k_shim.c を置き、patches/ を当て、
#   3. m68k-xelf で configure/make し、ssh.x (Human68k X 形式) を出す。
# 依存: elf2x68k (m68k-xelf-gcc)、libsocket.a は elf2x68k 同梱。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
VER=2022.83
SRC="$HERE/work/dropbear-$VER"
INC="$HERE/x68kinc"
OUT="${1:-$HERE/ssh.x}"
CROSS=m68k-xelf-
# FAST=1 で 68000 向け高速体演算 (net/ssh/crypto68k, vt100 agent 作) を組み込む。
# scalarmult 118s -> 10s (~12x)。FAST_C=1 なら asm 無し (22s)。CRYPTO68K で場所を指定可。
FAST="${FAST:-0}"
# DEBUG=1 で位相ごとの印 (x68dbg、経過秒付き) をテキスト画面に出す版を作る。
DEBUG="${DEBUG:-0}"
CRYPTO68K="${CRYPTO68K:-$HERE/../crypto68k}"

# パッチ適用前に必ず素の状態に戻す (前回のパッチが残っていると更新版が当たらない)。
TARBALL="$HERE/work/dropbear-$VER.tar.bz2"
if [ -f "$TARBALL" ]; then
  rm -rf "$SRC"; tar xjf "$TARBALL" -C "$HERE/work"
fi
[ -d "$SRC" ] || { echo "build: $SRC が無い。先に ./fetch.sh" >&2; exit 1; }

cp "$HERE/localoptions.h" "$SRC/localoptions.h"
cp "$HERE/x68k_shim.c"    "$SRC/x68k_shim.c"

EXTRA_DEF="" ; EXTRA_OBJ=""
[ "$DEBUG" = "1" ] && EXTRA_DEF="-DX68K_SSH_DEBUG"
if [ "$FAST" = "1" ]; then
  [ -f "$CRYPTO68K/field_fast.h" ] || { echo "build: FAST=1 だが $CRYPTO68K が無い (branch vt100 の net/ssh/crypto68k)" >&2; exit 1; }
  cp "$CRYPTO68K"/*.h "$SRC/"   # field_fast.h / comb.h / basetable.h 等
  if patch -d "$SRC" -p1 --dry-run -f < "$CRYPTO68K/curve25519.patch" >/dev/null 2>&1; then
    patch -d "$SRC" -p1 < "$CRYPTO68K/curve25519.patch"
  fi
  EXTRA_DEF="$EXTRA_DEF -DFAST_FIELD"
  [ "${FAST_C:-0}" != "1" ] && EXTRA_DEF="$EXTRA_DEF -DUSE_ASM"   # mul68k.o は make clean 後に組む
fi

# パッチ (冪等: 既に当たっていれば skip)
for p in "$HERE"/patches/*.patch; do
  f=$(basename "$p" .patch)
  if patch -d "$SRC" -p1 --dry-run -f < "$p" >/dev/null 2>&1; then
    patch -d "$SRC" -p1 < "$p"
  elif patch -d "$SRC" -p1 -R --dry-run -f < "$p" >/dev/null 2>&1; then
    echo "build: $f.patch は適用済み"
  else
    echo "build: $f.patch を当てられない" >&2; exit 1
  fi
done

cd "$SRC"
make clean >/dev/null 2>&1 || true
rm -f libtomcrypt/libtomcrypt.a libtommath/libtommath.a
# --host=m68k-elf で m68k-xelf-ar/ranlib を見つけさせる。_FORTIFY_SOURCE は無効 (ssp.h が無い)。
./configure --host=m68k-elf \
  --disable-zlib --disable-syslog --disable-lastlog \
  --disable-utmp --disable-utmpx --disable-wtmp --disable-loginfunc --disable-harden \
  CC=${CROSS}gcc AR=${CROSS}ar RANLIB=${CROSS}ranlib \
  CFLAGS="-m68000 -O2 -fomit-frame-pointer -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" \
  CPPFLAGS="-I$INC -include $INC/x68k_compat.h $EXTRA_DEF" \
  LIBS="-lsocket" >/dev/null

# configure がクロスで取り違える項目を直す
sed -i.bak 's|/\* #undef HAVE_BASENAME \*/|#define HAVE_BASENAME 1|' config.h
sed -i.bak 's|/\* #undef HAVE_SYS_UIO_H \*/|#define HAVE_SYS_UIO_H 1|' config.h
rm -f config.h.bak

# シム単体をコンパイル (make clean で消えるので make の前に用意し、LIBS に渡す)
${CROSS}gcc -m68000 -O2 -fomit-frame-pointer -I"$INC" -include "$INC/x68k_compat.h" $EXTRA_DEF -c x68k_shim.c -o x68k_shim.o
# FAST の asm 乗算も make clean 後に組む
case "$EXTRA_DEF" in *USE_ASM*) ${CROSS}as -m68000 "$CRYPTO68K/mul68k.s" -o "$SRC/mul68k.o"; EXTRA_OBJ="$SRC/mul68k.o";; esac

# 先に dry-run でリンク行を取っておく (ビルド後だと "up to date" で出ない)
LINK=$(make -n PROGRAMS="dbclient" LIBS="$SRC/x68k_shim.o $EXTRA_OBJ -lsocket" 2>/dev/null | grep -E "${CROSS}gcc +-o dbclient " | tail -1)
make PROGRAMS="dbclient" LIBS="$SRC/x68k_shim.o $EXTRA_OBJ -lsocket"
# 出力名を .x にして HU 形式に変換 + strip。取っておいたリンク行を再実行する。
[ -n "$LINK" ] || { echo "build: link 行が取れない" >&2; exit 1; }
sh -c "$(echo "$LINK" | sed "s|-o dbclient |-o '$OUT' -s |")"
echo "build: $OUT"
ls -la "$OUT"
