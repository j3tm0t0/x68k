# build-snippet.sh -- net/ssh/dropbear/build.sh に足す断片 (ssh ブランチには当てていない。手で 2 か所に挿す)
#
# ACCEL=1 で暗号アクセラレータ (net/accel: Pico W に X25519 / Ed25519 を任せる) を組み込む。組み込んでも既定は
# 使わない (ssh.x -oAccel=verify|kex|all か環境変数 ACCEL=... で有効化)。
#
# --- (1) FAST の if ブロックの後、patches/ を当てるループの前に:
ACCEL="${ACCEL:-0}"
ACCELDIR="${ACCELDIR:-$HERE/../../accel}"
if [ "$ACCEL" = "1" ]; then
  [ -f "$ACCELDIR/accel.c" ] || { echo "build: ACCEL=1 だが $ACCELDIR が無い (branch accel の net/accel)" >&2; exit 1; }
  cp "$ACCELDIR/accel.h" "$ACCELDIR/accel_proto.h" "$ACCELDIR/dropbear/x68k_accel.h" "$ACCELDIR/dropbear/x68k_accel.c" "$SRC/"
  if patch -d "$SRC" -p1 --dry-run -f < "$ACCELDIR/dropbear/dropbear-accel.patch" >/dev/null 2>&1; then
    patch -d "$SRC" -p1 < "$ACCELDIR/dropbear/dropbear-accel.patch"
  fi
  EXTRA_DEF="$EXTRA_DEF -DX68K_ACCEL=1"
fi
# --- (2) x68k_shim.o / vt100.o をコンパイルした後 (make の前) に:
if [ "$ACCEL" = "1" ]; then
  ${CROSS}gcc -m68000 -O2 -fomit-frame-pointer -I"$ACCELDIR" -c x68k_accel.c -o x68k_accel.o
  ${CROSS}gcc -m68000 -O2 -fomit-frame-pointer -I"$ACCELDIR" -c "$ACCELDIR/accel.c" -o accel.o
  ${CROSS}gcc -m68000 -c "$ACCELDIR/accel_etd.S" -o accel_etd.o
  EXTRA_OBJ="$EXTRA_OBJ $SRC/x68k_accel.o $SRC/accel.o $SRC/accel_etd.o"
fi
