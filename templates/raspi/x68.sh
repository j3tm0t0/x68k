#!/bin/bash
# x68 'ls -l /usr/bin' : PPP でつながった X68000 (rcmd.x 起動中) でコマンドを実行し、結果を表示する
# 終了コードは X68000 側の終了コード。X68_HOST / X68_PORT で接続先を変えられる。bash の /dev/tcp を使うので nc 不要。
# コマンドは UTF-8 -> Shift_JIS にして送り、出力は Shift_JIS/CRLF -> UTF-8/LF に直して表示する。
HOST=${X68_HOST:-192.168.68.2}
PORT=${X68_PORT:-2323}
[ $# -gt 0 ] || { echo "usage: x68 <command line for zsh on X68000>" >&2; exit 2; }
exec 3<>"/dev/tcp/$HOST/$PORT" || exit 1
printf '%s\r\n' "$*" | iconv -f UTF-8 -t CP932 >&3
rc=1
# パイプにすると while がサブシェルで動いて rc が戻らないので、出力変換はプロセス置換で行う
exec 4> >(iconv -f CP932 -t UTF-8 -c | tr -d '\r')
while IFS= read -r line <&3 || [ -n "$line" ]; do
  case "$line" in
    --rcmd-end\ rc=*--*) rc=${line#--rcmd-end rc=}; rc=${rc%%--*}; break;;
  esac
  printf '%s\n' "$line" >&4
done
exec 4>&- 3>&-
wait 2>/dev/null
exit "${rc:-1}"
