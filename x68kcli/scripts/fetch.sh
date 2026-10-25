#!/usr/bin/env bash
# sources.txt に列挙したアーカイブを dl/ に取得し sha256 を検証する
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dl
# CI_ARTIFACTS=ディレクトリ: 非公開の S3 から落としたミラー (x68kcli-dl-*.tar.gz) があれば、dl/ にまだ無いものだけ先に展開する。
# 外のサイトへは行かない。sha256 の検証は下でこれまでどおり
if [ -n "${CI_ARTIFACTS:-}" ]; then
  for t in "$CI_ARTIFACTS"/x68kcli-dl-*.tar.gz; do [ -f "$t" ] && tar xzkf "$t" -C . 2>/dev/null || true; done
fi
fail=0
while read -r dest url sha; do
  case "$dest" in ''|'#'*) continue;; esac
  out="dl/$dest"
  mkdir -p "$(dirname "$out")"
  if [ ! -s "$out" ]; then
    echo "fetch  $url"
    curl -fsSL --retry 3 -o "$out" "$url" || { echo "  !! download failed: $url"; fail=1; continue; }
  fi
  got=$(shasum -a 256 "$out" | cut -c1-64)
  if [ "$got" != "$sha" ]; then
    echo "  !! sha256 mismatch: $out"
    echo "     expected $sha"
    echo "     got      $got"
    [ "${SKIP_SHA:-0}" = 1 ] || fail=1
  fi
done < sources.txt
[ "$fail" = 0 ] || { echo "fetch: errors occurred"; exit 1; }
echo "fetch: ok"
