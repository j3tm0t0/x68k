#!/bin/sh
# 段階 4 (SLIP) で X68000 に送るファイルを files-net/ に用意する。
#   xip.x (ESP/X の TCP/IP、K.Shirakata 氏の改良版 β5) と ifconfig.x / inetdconf.x / protocols / services (計測技研 TCPPACKA) を
#   retropc.net から取り、xip.x には net/patchxip.py で MSS 1460 などのパッチをあてる (改変済みのバイナリは配布しない)。
#   slip.sys / slipd.x は net/slip, net/slipd を elf2x68k でビルドする (Mac か、elf2x68k の入った Linux で)。
#   rcmd.x は X68000 の上でビルドするもの (harness/rcmd) なので、RCMD_X=パス で渡すか、x68kcli の HDS イメージから取り出す。
# 要るもの: curl、lha か lhasa、shasum か sha256sum、python3 (rcmd.x を HDS から取るなら rb-cli)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=$here/files-net
mkdir -p "$out/.tmp"
cd "$out/.tmp"
fetch() {   # fetch URL SHA256
  f=$(basename "$1")
  [ -s "$f" ] || curl -fsSL -o "$f" "$1"
  got=$( (sha256sum "$f" 2>/dev/null || shasum -a 256 "$f") | cut -d' ' -f1)
  [ "$got" = "$2" ] || { echo "getnet: sha256 mismatch for $f" >&2; exit 1; }
}
unlzh() { if command -v lha >/dev/null; then lha xqf "$1"; else lhasa xf "$1"; fi; }
fetch http://retropc.net/x68000/software/internet/tcpip/xip/xipb5.lzh 4d35da4a66b50d863a9c84ec230d61d98ae544edce4f7a21b1df69b60293cc84
fetch http://retropc.net/x68000/software/internet/kg/tcppacka/TCPPACKA.LZH ff8e18638b5349d175954f885a9bdb469395ba84a17382441f6b4e6543b3e91b
mkdir -p xip tcpa
(cd xip && unlzh ../xipb5.lzh >/dev/null)
(cd tcpa && unlzh ../TCPPACKA.LZH >/dev/null)
python3 "$top/net/patchxip.py" "$(find xip -iname xip.x | head -1)" "$out/XIP.X" --mss 1460 --timer --yield
cp "$(find tcpa -iname ifconfig.x | head -1)" "$out/IFCONFIG.X"
cp "$(find tcpa -iname inetdconf.x | head -1)" "$out/INETDCONF.X"
cp "$(find tcpa -iname protocols | head -1)" "$out/protocols"
cp "$(find tcpa -iname services | head -1)" "$out/services"
make -s -C "$top/net/slip" slip.sys && cp "$top/net/slip/slip.sys" "$out/SLIP.SYS"
make -s -C "$top/net/slipd" slipd.x && cp "$top/net/slipd/slipd.x" "$out/SLIPD.X"
if [ -n "${RCMD_X:-}" ]; then
  cp "$RCMD_X" "$out/RCMD.X"
else
  rb=${RBCLI:-$(command -v rb-cli || echo "$top/x68kcli/tools/rb-cli")}
  [ -x "$rb" ] || rb=/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/rb-cli
  for h in ${RCMD_HDS:-} "$top/x68kcli/build/xipbase.hds" "$top/x68kcli/build/x68kcli.hds" \
           /Users/moto/Dropbox/src/github.com/j3tm0t0/x68k/x68kcli/build/xipbase.hds; do
    [ -f "$h" ] && [ -x "$rb" ] && "$rb" get "$h" /USR/BIN/rcmd.x "$out/RCMD.X" >/dev/null 2>&1 && break
  done
fi
[ -s "$out/RCMD.X" ] || { echo "getnet: rcmd.x not found (RCMD_X=パス で渡す)" >&2; exit 1; }
cd "$out" && rm -rf .tmp
ls -l "$out"
