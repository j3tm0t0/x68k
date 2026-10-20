#!/usr/bin/env bash
# ホスト側ツール (rb-cli, XEiJ) を tools/ に導入する。lha と Java は Homebrew から。
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tools
RB_VER=2026-10-11-23-59
RB_SHA=c647364e31cdd763eadd85c20d899fcc0f8bc6a70a47ccf0bd84f253e35fb608
XEIJ_ZIP=XEiJ_0261008.zip

case "$(uname -sm)" in
  "Darwin arm64") rb_asset="rb-cli-macos-arm64-$RB_VER.zip";;
  "Darwin x86_64") rb_asset="rb-cli-macos-x64-$RB_VER.zip"; RB_SHA=;;
  "Linux x86_64") rb_asset="rb-cli-linux-x64-$RB_VER.tar.gz"; RB_SHA=;;
  "Linux aarch64") rb_asset="rb-cli-linux-arm64-$RB_VER.tar.gz"; RB_SHA=;;
  *) echo "unsupported host: $(uname -sm)"; exit 1;;
esac

if [ ! -x tools/rb-cli ]; then
  echo "fetch  rb-cli ($rb_asset)"
  curl -fsSL -o "tools/$rb_asset" "https://github.com/danifunker/rusty-backup/releases/download/v$RB_VER/$rb_asset"
  if [ -n "$RB_SHA" ]; then
    got=$(shasum -a 256 "tools/$rb_asset" | cut -c1-64)
    [ "$got" = "$RB_SHA" ] || { echo "rb-cli sha256 mismatch ($got)"; exit 1; }
  fi
  case "$rb_asset" in
    *.zip) unzip -oq "tools/$rb_asset" -d tools;;
    *.tar.gz) tar xzf "tools/$rb_asset" -C tools;;
  esac
  [ -x tools/rb-cli ] || { f=$(find tools -name rb-cli -type f | head -1); mv "$f" tools/rb-cli; }
  chmod +x tools/rb-cli
fi
tools/rb-cli --version

if [ ! -f tools/xeij/XEiJ.jar ]; then
  echo "fetch  XEiJ ($XEIJ_ZIP)"
  mkdir -p tools/xeij
  curl -fsSL -o "tools/xeij/$XEIJ_ZIP" "https://stdkmd.net/xeij/$XEIJ_ZIP"
  unzip -oq "tools/xeij/$XEIJ_ZIP" -d tools/xeij
fi

command -v lha >/dev/null || echo "NOTE: lha が見つかりません: brew install lha"
