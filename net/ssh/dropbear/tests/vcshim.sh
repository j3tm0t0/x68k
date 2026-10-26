#!/bin/bash
# 仮想コンソール (x68k_vcons.c) のホスト試験を組んで流す (tests/vcshim.c)。ASan/UBSan 付き。
#   要: term/zterm/server の cargo build --release (フレーム列を作る)
#   Linux の ASan は終了時に解放していないメモリも数える (Mac では数えない)。仮想コンソールの画面は終わるまで持つものなので、
#   それは見ない (detect_leaks=0)。
set -eu
export ASAN_OPTIONS="${ASAN_OPTIONS:-}${ASAN_OPTIONS:+:}detect_leaks=0"
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
