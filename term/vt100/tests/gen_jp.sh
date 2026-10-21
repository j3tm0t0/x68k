#!/usr/bin/env bash
# 日本語フィクスチャ (全角込み) を作る。TERM=vt100 / 80x24。
set -euo pipefail
cd "$(dirname "$0")"
PY=./venv/bin/python
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

"$PY" -I - "$W/jp.txt" <<'PY'
import sys
lines=[]
for i in range(1,61):
    lines.append("%03d 日本語テスト：吾輩は猫である。名前はまだ無い。ABCあいうえおカタカナ漢字混在"%i)
open(sys.argv[1],"w",encoding="utf-8").write("\n".join(lines)+"\n")
PY

# 1) cat: 全角の折り返しを試す
cat > "$W/cat.script" <<EOF
send cd "$W"\r
wait 200
send cat jp.txt\r
until 800
EOF
COLUMNS=80 LINES=24 "$PY" -I record.py fixtures/jp_cat.rec 80 24 vt100 "$W/cat.script" -- /bin/bash --norc --noprofile -i

# 2) less: スクロール + 全角
cat > "$W/less.script" <<'EOF'
wait 500
send f
until 400
send f
until 400
send G
until 500
EOF
"$PY" -I record.py fixtures/jp_less.rec 80 24 vt100 "$W/less.script" -- less -X "$W/jp.txt"

ls -la fixtures/jp_*.rec
