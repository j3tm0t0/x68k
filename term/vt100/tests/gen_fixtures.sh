#!/usr/bin/env bash
# フィクスチャ (VT100 バイト列の記録) を作る。決定的にするため専用の作業ディレクトリを用意する。
# TERM=vt100 / 80x24 で記録する (代替画面 1049 を使わせず pyte と条件を揃える)。
set -euo pipefail
cd "$(dirname "$0")"
PY=./venv/bin/python
REC="$PY -I record.py"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# ---- ls --color 用の題材 ----
mkdir -p "$W/dir1" "$W/dir2"
touch "$W/afile.txt" "$W/bfile.c" "$W/zscript.sh"
chmod +x "$W/zscript.sh"
ln -s afile.txt "$W/alink"
printf 'alpha\nbravo\ncharlie\n' > "$W/afile.txt"

# ---- vim 用の長めのファイル ----
seq 1 200 | awk '{printf "%3d: the quick brown fox jumps over the lazy dog line\n",$1}' > "$W/long.txt"

# 色付き ls。-l は付けない (所有者名・日時が入り非決定的になるため)。ファイル名はこちらで作った決定的なもの。
if command -v gls >/dev/null; then LSCMD="gls --color=always -F"; else LSCMD="ls -GF"; fi

cat > "$W/ls.script" <<EOF
send cd "$W"\r
wait 200
send export PS1='user@x68k\\$ '\r
wait 300
send clear\r
wait 300
send $LSCMD\r
until 600
EOF
# shell + ls --color
CLICOLOR_FORCE=1 COLUMNS=80 LINES=24 $REC fixtures/shell.rec 80 24 vt100 "$W/ls.script" -- \
  /bin/bash --norc --noprofile -i || true
( cd "$W" && :)

cat > "$W/vim.script" <<'EOF'
wait 600
send :set nonu noai\r
wait 200
send G
until 400
send 1G
until 400
send 15Gzz
until 400
send jjjjjjjjjj
until 500
send /fox\r
until 500
EOF
VISUAL= EDITOR= $REC fixtures/vim.rec 80 24 vt100 "$W/vim.script" -- vim -u NONE -N "$W/long.txt" || true

cat > "$W/less.script" <<'EOF'
wait 500
send f
until 400
send f
until 400
send f
until 400
send G
until 500
send g
until 500
EOF
$REC fixtures/less.rec 80 24 vt100 "$W/less.script" -- less -X "$W/long.txt" || true

cat > "$W/top.script" <<'EOF'
wait 1500
until 1200
until 1200
until 1200
send q
until 400
EOF
$REC fixtures/top.rec 80 24 vt100 "$W/top.script" -- top || true
# top の記録からユーザ名を消す (同じ長さの語に置換してバイト数・桁を保つ)。個人情報をコミットしないため。
U=$(id -un)
$PY -I - fixtures/top.rec "$U" <<'PY'
import sys
path, user = sys.argv[1], sys.argv[2].encode()
d = open(path, "rb").read()
repl = b"user"
if len(repl) < len(user): repl = repl + b"x" * (len(user) - len(repl))
repl = repl[:len(user)]
open(path, "wb").write(d.replace(user, repl))
PY

ls -la fixtures/*.rec
