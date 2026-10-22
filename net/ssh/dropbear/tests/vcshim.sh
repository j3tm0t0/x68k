#!/bin/bash
# 仮想コンソール (x68k_vcons.c) のホスト試験を組んで流す (tests/vcshim.c)。ASan/UBSan 付き。
#   要: term/zterm/server の cargo build --release (フレーム列を作る)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
TERM_="$HERE/../../../../term"
VT="$TERM_/vt100"; ZC="$TERM_/zterm/client"; Z="$TERM_/zterm/server/target/release/zterm"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
[ -x "$Z" ] || (cd "$TERM_/zterm/server" && cargo build --release -q)
for b in app_top app_vimjp; do "$Z" --replay "$VT/tests/fixtures/$b.rec" --cols 96 --rows 32 --chunk 64 --out "$T/$b.zbin" 2>/dev/null; done
cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wno-unused-function -Wno-deprecated-declarations \
  -DVT_HOSTTEST -DVT_VCONS -DVT_HOSTKEYS -DX68K_ZTERM=1 -I"$VT" -I"$ZC" \
  -o "$T/vcshim" "$HERE/vcshim.c" "$ZC/ztdraw.c" "$ZC/ztclient.c" 2>&1 | grep -v 'reducing alignment' || true
"$T/vcshim" "$T/app_top.zbin" "$T/app_vimjp.zbin"
