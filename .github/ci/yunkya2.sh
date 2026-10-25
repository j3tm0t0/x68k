#!/usr/bin/env bash
# CI: harness/picosim が読む yunkya2 氏の公開ソースを固定コミットで取ってくる。
#   DEST/x68kzremotedrv   yunkya2/x68kzremotedrv 3afd408 + net/x68kzremotedrv-netpipe/patches (git am --keep-cr)、
#                         x68kserremote (サブモジュール)、driver/ の 68000 のバイナリ (elf2x68k でビルド)
#                         -> FW=DEST/x68kzremotedrv/driver  RS=DEST/x68kzremotedrv/x68kserremote
#   DEST/lwip             lwIP STABLE-2_2_0_RELEASE (pico-sdk 2.0.0 の lib/lwip と同じコミット) -> LWIP=DEST/lwip
# 使い方: yunkya2.sh DEST [lwip|fw]...   (既定は両方。fw は m68k-xelf-gcc が要る)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
DEST=$1; shift
mkdir -p "$DEST"; DEST=$(cd "$DEST" && pwd)
ZRD_REV=3afd408df99f3f9d1aafc7c090e54285cbeadba6
LWIP_REV=0a0452b2c39bdd91e252aef045c115f88f6ca773
fetch() {   # fetch URL REV DIR
  rm -rf "$3"; git init -q "$3"
  git -C "$3" fetch -q --depth 1 "$1" "$2"
  git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
  [ "$(git -C "$3" rev-parse HEAD)" = "$2" ]
}
for what in "${@:-lwip fw}"; do for w in $what; do case $w in
lwip)
  fetch https://github.com/lwip-tcpip/lwip.git $LWIP_REV "$DEST/lwip"
  echo "yunkya2.sh: lwIP $LWIP_REV -> $DEST/lwip" ;;
fw)
  Z=$DEST/x68kzremotedrv
  fetch https://github.com/yunkya2/x68kzremotedrv.git $ZRD_REV "$Z"
  git -C "$Z" -c user.name=ci -c user.email=ci@localhost am -q --keep-cr "$TOP"/net/x68kzremotedrv-netpipe/patches/*.patch
  git -C "$Z" submodule update -q --init --depth 1 x68kserremote
  make -s -C "$Z/driver" bootwait1.bin bootwait.bin hdsboot.bin settingui.bin scsiremote.bin
  echo "yunkya2.sh: x68kzremotedrv $ZRD_REV + $(ls "$TOP"/net/x68kzremotedrv-netpipe/patches/*.patch | wc -l | tr -d ' ') patches -> $Z (driver/*.bin)" ;;
*) echo "yunkya2.sh: unknown $w"; exit 1 ;;
esac; done; done
