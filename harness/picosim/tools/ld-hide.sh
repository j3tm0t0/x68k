#!/bin/sh
# ld-hide.sh OUT.o EXPORTS IN.o... -- いくつかの .o を 1 個にまとめ (ld -r)、EXPORTS (1 行 1 名、前に _ を付けない) 以外の
# 名前を外から見えなくする。lwIP と libslirp のように同じ名前を持つものを 1 つの picosim にリンクするため。
# macOS は ld の -exported_symbols_list、Linux は ld -r のあと objcopy --keep-global-symbols (constructor は名前が要らない)
set -e
out=$1; ex=$2; shift 2
if [ "$(uname)" = Darwin ]; then
  sed 's/^/_/' "$ex" > "$ex.macho"
  ld -r -o "$out" "$@" -exported_symbols_list "$ex.macho"
else
  ld -r -o "$out" "$@"
  # 空の一覧を渡すと objcopy は失敗するので、そのときは全部を局所に
  if [ -s "$ex" ]; then objcopy --keep-global-symbols="$ex" "$out"; else objcopy --wildcard --localize-symbol='*' "$out"; fi
fi
