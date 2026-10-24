#!/bin/sh
# files/LHA.X を用意する: LHA for X680x0 (Norio OKADA) の自己解凍ファイルを retropc.net から取り、中の LHA.x を取り出す。
# 自己解凍ファイルをそのまま X68000 で実行すると上書きの確認でキー入力を待つので、Pi / Mac で取り出してから送る。
# 要るもの: curl、lha か lhasa、shasum か sha256sum
set -eu
cd "$(dirname "$0")"
url=http://retropc.net/x68000/software/tools/archiver/lha/LHA_X662.x
sum=7801d19a0f351aeb26dc6121774fd68f15dfbdf25b97a3bb53851d058eb86753
mkdir -p files/.tmp
[ -s files/.tmp/LHA_X662.x ] || curl -fsSL -o files/.tmp/LHA_X662.x "$url"
got=$( (sha256sum files/.tmp/LHA_X662.x 2>/dev/null || shasum -a 256 files/.tmp/LHA_X662.x) | cut -d' ' -f1)
[ "$got" = "$sum" ] || { echo "getlha: sha256 mismatch: $got" >&2; exit 1; }
cd files/.tmp
if command -v lha >/dev/null; then lha xqf LHA_X662.x LHA.x; else lhasa xf LHA_X662.x LHA.x; fi
mv LHA.x ../LHA.X
cd .. && rm -rf .tmp
ls -l LHA.X
