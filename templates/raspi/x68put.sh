#!/bin/bash
# x68put <local file> <X68000 path> : rcmd.x 起動中の X68000 にファイルを送る
#   4KB を超えるファイルは tar + gzip で圧縮して送り、X68000 側の tar.x -xzf で展開する
#   (X68000 側の処理が頭打ちになるので (PPP 約 1KB/s、SLIP 38400 bps で約 2.7KB/s)、送るバイト数を減らすのが効く)
#   X68PUT_RAW=1  : 圧縮しない
#   X68PUT_TEXT=1 : テキストを Human68k 用 (Shift_JIS / CRLF / 末尾 ^Z) にしてから送る
HOST=${X68_HOST:-192.168.68.2}
PORT=${X68_PORT:-2323}
[ $# -eq 2 ] || { echo "usage: x68put <local file> <X68000 path>" >&2; exit 2; }
tmpd=$(mktemp -d); trap 'rm -rf "$tmpd"' EXIT
src=$1 dst=$2
if [ -n "${X68PUT_TEXT:-}" ]; then
  # Human68k のテキスト: Shift_JIS + CRLF + 末尾 ^Z (^Z が無いと gcc 1.42 の cpp がバスエラーで落ちた)
  { iconv -f UTF-8 -t CP932 "$src" | LC_ALL=C sed 's/$/\r/'; printf '\032'; } > "$tmpd/text"; src=$tmpd/text
fi
size=$(stat -c %s "$src")

# send <file> <X68000 path> : @put で 1 ファイル送り、終了コードを返す
send() {
  exec 3<>"/dev/tcp/$HOST/$PORT" || return 1
  { printf '@put %s %s\r\n' "$2" "$(stat -c %s "$1")"; cat "$1"; } >&3
  local rc=1 line
  while IFS= read -r line <&3 || [ -n "$line" ]; do
    case "$line" in --rcmd-end\ rc=*--*) rc=${line#--rcmd-end rc=}; rc=${rc%%--*}; break;; esac
  done
  exec 3>&-
  return "${rc:-1}"
}

t0=$(date +%s.%N)
how=raw; wire=$size
if [ -z "${X68PUT_RAW:-}" ] && [ "$size" -gt 4096 ]; then
  # 展開先のファイル名で 1 ファイルだけの tar.gz を作る (tar 1.11 なので GNU 形式・数値 owner)
  name=$(basename "$dst"); dir=$(dirname "$dst")
  mkdir "$tmpd/a"; cp "$src" "$tmpd/a/$name"
  tar -C "$tmpd/a" --format=gnu --owner=0 --group=0 -czf "$tmpd/x.tgz" "$name"
  if [ "$(stat -c %s "$tmpd/x.tgz")" -lt $((size * 9 / 10)) ]; then
    how=gzip; wire=$(stat -c %s "$tmpd/x.tgz")
    send "$tmpd/x.tgz" /tmp/x68put.tgz || { echo "x68put: send failed" >&2; exit 1; }
    # 展開 (X68000 側の zsh で実行)
    out=$("$(dirname "$0")/x68" "cd $dir && tar.x -xzf /tmp/x68put.tgz && rm /tmp/x68put.tgz"); rc=$?
    [ $rc = 0 ] || { echo "x68put: extract failed rc=$rc: $out" >&2; exit "$rc"; }
  fi
fi
if [ $how = raw ]; then
  send "$src" "$dst"; rc=$?
  [ $rc = 0 ] || { echo "x68put: failed rc=$rc" >&2; exit "$rc"; }
fi
t=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
eff=$(awk -v s="$size" -v t="$t" 'BEGIN{printf "%.0f", (t>0)? s/t : 0}')
kbps=$(awk -v e="$eff" 'BEGIN{printf "%.1f", e*8/1000}')
if [ $how = gzip ]; then
  echo "x68put: $1 -> $2 ($size bytes, gzip $wire bytes, ${t}s, ${eff} B/s = ${kbps} kbps effective)"
else
  echo "x68put: $1 -> $2 ($size bytes, ${t}s, ${eff} B/s = ${kbps} kbps)"
fi
