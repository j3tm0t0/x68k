#!/usr/bin/env bash
# 空の Human68k SCSI HDD イメージを作り、stage/ の内容 (システムファイル込み) を書き込む
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=${IMG:-build/x68kcli.hds}
SIZE_MB=${SIZE_MB:-100}
mkdir -p build
python3 -I scripts/mkhds.py stage/BIN/FORMAT.X "$IMG" "$SIZE_MB"
python3 -I scripts/mkbatch.py stage "$IMG" > build/batch.json
tools/rb-cli batch --progress never build/batch.json | tail -1
# rb-cli が空白埋めするディレクトリエントリの拡張名を 0 埋めに直す (Human68k が HUMAN.SYS を見つけられない)
python3 -I scripts/fixfs.py "$IMG"
echo "built: $IMG"
