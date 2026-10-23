#!/bin/bash
# build-pzl.sh <pzl のコミット> <出力の .elf> [cmake の -D...]: Pico Z Loader の採取用ビルド
# (TinyUSB のログ CFG_TUSB_DEBUG=2 を ms 付き・決まりきった行を省いて UART に、READ/WRITE(10) の番地も)。
# 例: build-pzl.sh 35fd5d8 /tmp/pzlcap-mouse.elf -DPZL_UDP_KEYS=ON -DPZL_HID_MOUSE=ON
# 環境変数: PZLREPO (x68k-zos の clone、既定 ../x68k-zos-pzl)、PICO_SDK_PATH、PICO_TOOLCHAIN_PATH、BOARD (既定 pico2_w)
set -euo pipefail
rev=$1 out=$2; shift 2
here=$(cd "$(dirname "$0")" && pwd)
repo=${PZLREPO:-$here/../../../../x68k-zos-pzl}
: "${PICO_SDK_PATH:?set PICO_SDK_PATH}"
w=$(mktemp -d "${TMPDIR:-/tmp}/pzlcap.XXXXXX")
git -C "$repo" archive "$rev" pico | tar -x -C "$w"
git -C "$repo" ls-tree -r "$rev" pico | awk '$2=="commit"{print $3, $4}' | while read -r c p; do
  git -C "$repo/$p" archive "$c" | tar -x -C "$w/$p"; done
cp "$here/zcap.c" "$w/pico/src/usb/"
(cd "$w" && patch -s -p1 < "$here/pzl-zcap.patch")
board=${BOARD:-pico2_w}; plat=rp2040; [ "$board" = pico2_w ] && plat=rp2350-arm-s
cmake -S "$w/pico" -B "$w/b" -DPICO_SDK_PATH="$PICO_SDK_PATH" -DPICO_BOARD=$board -DPICO_PLATFORM=$plat \
  -DCMAKE_BUILD_TYPE=Release "-DPZL_EXTRA_DEFS=CFG_TUSB_DEBUG=2;CFG_TUSB_DEBUG_PRINTF=zcap_printf" "$@" > "$w/cmake.log"
cmake --build "$w/b" -j8 > "$w/build.log" 2>&1 || { tail -20 "$w/build.log"; exit 1; }
cp "$w/b/pzl.elf" "$out"; rm -rf "$w"; echo "$out"
