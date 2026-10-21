#!/usr/bin/env bash
# 絵文字フィクスチャ。pyte が素直に幅2で扱う絵文字 (astral + ⭐✨) のみ使う。TERM=vt100 / 80x24。
set -euo pipefail
cd "$(dirname "$0")"
PY=./venv/bin/python
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

"$PY" -I - "$W/emoji.txt" <<'PY'
import sys
em=["\U0001F600","\U0001F680","\U0001F44D","\U0001F389","⭐","✨","\U0001F525","\U0001F4AF"]
lines=[]
for i in range(1,41):
    e=em[i%len(em)]; e2=em[(i*3)%len(em)]
    lines.append("%03d: hello %s world %s end ABCDEF 0123456789 %s%s%s tail"%(i,e,e2,e,e2,em[(i*5)%len(em)]))
open(sys.argv[1],"w",encoding="utf-8").write("\n".join(lines)+"\n")
PY

cat > "$W/cat.script" <<EOF
send cd "$W"\r
wait 200
send cat emoji.txt\r
until 800
EOF
COLUMNS=80 LINES=24 "$PY" -I record.py fixtures/emoji_cat.rec 80 24 vt100 "$W/cat.script" -- /bin/bash --norc --noprofile -i

cat > "$W/less.script" <<'EOF'
wait 500
send f
until 400
send f
until 400
send G
until 500
EOF
"$PY" -I record.py fixtures/emoji_less.rec 80 24 vt100 "$W/less.script" -- less -X "$W/emoji.txt"

ls -la fixtures/emoji_*.rec
