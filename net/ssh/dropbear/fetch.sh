#!/bin/bash
# dropbear の配布 tarball を取得して SHA-256 を確かめ、work/ に展開する。
# 改変済みのソースはこのリポジトリに置かない (配布物は配布条件に従う)。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/work"
VER=2022.83
TARBALL="dropbear-$VER.tar.bz2"
SHA256="bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b"
URL1="https://matt.ucc.asn.au/dropbear/releases/$TARBALL"
URL2="https://github.com/mkj/dropbear/releases/download/DROPBEAR_$VER/$TARBALL"

mkdir -p "$WORK"
cd "$WORK"
if [ ! -f "$TARBALL" ]; then
  curl -fsSL -o "$TARBALL" "$URL1" || curl -fsSL -o "$TARBALL" "$URL2"
fi
# SHA-256 を確認 (macOS: shasum / Linux: sha256sum)
if command -v sha256sum >/dev/null; then GOT=$(sha256sum "$TARBALL" | cut -d' ' -f1);
else GOT=$(shasum -a 256 "$TARBALL" | cut -d' ' -f1); fi
if [ "$GOT" != "$SHA256" ]; then
  echo "fetch: SHA-256 mismatch for $TARBALL" >&2
  echo "  expected $SHA256" >&2
  echo "  got      $GOT" >&2
  exit 1
fi
rm -rf "dropbear-$VER"
tar xjf "$TARBALL"
echo "fetch: dropbear-$VER extracted into $WORK (SHA-256 OK)"
