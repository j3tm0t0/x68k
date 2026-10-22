#!/usr/bin/env bash
# ホスト試験用の CGROM イメージ (768KB、X68000 の 0xF00000-0xFBFFFF) を tests/cgrom/CGROM.DAT に用意する。
# XEiJ 同梱の CGROM_XEiJ.DAT (XEiJ は -cgrom 指定が無ければこれを使う) を写す。配布物なので git には入れない。
#   CGROM_SRC=... で場所を指定できる。無ければ XEiJ の配布 zip の data/CGROM_XEiJ.DAT を取り出して置くこと。
#   実機の ROM を使うなら、XEiJ/実機で 0xF00000 から 0xC0000 バイトをダンプしたものでもよい
#   (XEiJ: x68kcli の scripts/xeijcmd.sh dump FILE 0xF00000 786432)。
set -euo pipefail
cd "$(dirname "$0")"
SRC=${CGROM_SRC:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/data/CGROM_XEiJ.DAT}
[ -f "$SRC" ] || { echo "CGROM が見つからない: $SRC (CGROM_SRC=... で指定)"; exit 1; }
mkdir -p cgrom
cp "$SRC" cgrom/CGROM.DAT
[ "$(wc -c < cgrom/CGROM.DAT)" -eq 786432 ] || { echo "CGROM の大きさが 768KB でない"; exit 1; }
echo "tests/cgrom/CGROM.DAT <- $SRC"
