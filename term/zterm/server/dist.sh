#!/bin/bash
# zterm ヘルパを 4 アーキテクチャ分ビルドし、Z の B: に置く形 (<arch>.gz と VERSION) を ../dist に作る。
#   linux-aarch64.gz linux-x86_64.gz darwin-arm64.gz darwin-x86_64.gz VERSION
# 名前は起動スクリプトの `uname -s`-`uname -m` を小文字にしたもの (Human68k の 18+3 文字に収まる)。
# musl は rustup 同梱の self-contained libc と rust-lld で静的リンク (.cargo/config.toml)。
# VERSION は「Cargo の版 + 4 本の中身のハッシュ」: 作り直すと変わり、リモートのキャッシュが入れ替わる。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../dist}"
mkdir -p "$OUT"
cd "$HERE"
declare -a T=(aarch64-unknown-linux-musl:linux-aarch64 x86_64-unknown-linux-musl:linux-x86_64 aarch64-apple-darwin:darwin-arm64 x86_64-apple-darwin:darwin-x86_64)
for t in "${T[@]}"; do
  tri=${t%%:*}; name=${t##*:}
  cargo build --release --quiet --target "$tri"
  cp "target/$tri/release/zterm" "$OUT/$name"
  gzip -9 -n -c "$OUT/$name" > "$OUT/$name.gz"
done
ver=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
h=$(cat "$OUT"/*.gz | shasum -a 256 | cut -c1-8)
echo "$ver-$h" > "$OUT/VERSION"
cd "$OUT"
ls -l VERSION *.gz | awk '{print $5, $9}'
for t in "${T[@]}"; do n=${t##*:}; printf '%s %s raw\n' "$(wc -c < $n | tr -d ' ')" "$n"; done
cat VERSION
