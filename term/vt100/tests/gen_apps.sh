#!/usr/bin/env bash
# 実アプリのフィクスチャを Mac 上で記録する。96x32 (X68000 の既定)、TERM=xterm (ssh.x が名乗る値)。
#   app_tmux     tmux (状態行・左右/上下分割・ペインごとの出力)
#   app_claude   Claude Code (TUI の起動画面 → hello と答えさせて /exit。道具は使わせない)
#   app_claude_help  claude --help
#   app_vimjp    vim で日本語ファイル (全角・カーソル移動・挿入)
#   app_top      top
#   app_lessu8   less で UTF-8 (全角・罫線・記号)
#   app_gitlog   git log --graph --color (このリポジトリの履歴、作者名は出さない書式)
#   app_emoji    絵文字と全角/半角の境界
# 記録には個人情報 (ユーザ名・ホスト名・ホームのパス・メール) が混ざるので、同じ長さの語に置き換える (scrub.py)。
#   app_claude_stream  Claude Code に 80 行の応答を流させる (応答中の描き直し)
# 使い方: tests/gen_apps.sh [名前...]   (既定は全部。API を呼ぶ app_claude / app_claude_stream は明示したときだけ)
set -euo pipefail
cd "$(dirname "$0")"
PY="$PWD/venv/bin/python -W ignore"
REC="$PY -I $PWD/record.py"
W=$(mktemp -d)
trap 'tmux -S "$W/tmux.sock" kill-server 2>/dev/null || true; rm -rf "$W"' EXIT
COLS=96; ROWS=32
unset TMUX TMUX_PANE
export COLUMNS=$COLS LINES=$ROWS
TARGETS=("$@")
sel() {   # 引数なしなら app_claude (API を呼ぶ) 以外の全部
  if [ ${#TARGETS[@]} -eq 0 ]; then [ "$1" != app_claude ] && [ "$1" != app_claude_stream ]; return; fi
  local x; for x in "${TARGETS[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1
}
sizes_add() { awk -v n="$1" '$1 != n' fixtures/sizes.txt > "$W/sizes" 2>/dev/null || true; echo "$1 $COLS $ROWS" >> "$W/sizes"; sort "$W/sizes" > fixtures/sizes.txt; }

# ---- 題材 ----
$PY -I - "$W" <<'PY'
import sys, os
w = sys.argv[1]
jp = []
for i in range(1, 41):
    jp.append("%02d 漢字かなカナ混じり文：通信は無事に終わった。危険物は無い。ABC abc 123" % i)
open(os.path.join(w, "jp.txt"), "w", encoding="utf-8").write("\n".join(jp) + "\n")
u8 = ["┌───────┬───────┐", "│ 日本語 │ ascii │", "├───────┼───────┤", "│ 全角ｶﾅ │ ±≤≥→← │", "└───────┴───────┘",
      "• bullet · middot … ellipsis — dash “quotes” ‘single’", "✓ check ✗ cross ⏺ record ⎿ corner ✻ star ❯ prompt",
      "café naïve e\u0301 combining", "Ω≈ç√∫˜µ≤≥÷ ¡™£¢∞§¶•ªº", "한국어 中文 русский ελληνικά"]   # ZWSP・VS16 は pyte が行の残りを捨てるので syn_edge で試す
u8 += ["line %03d %s" % (i, "日本" * (i % 7)) for i in range(1, 60)]
open(os.path.join(w, "u8.txt"), "w", encoding="utf-8").write("\n".join(u8) + "\n")
# 異体字セレクタ・ZWJ・肌色・右端は pyte が xterm と違うので syn_edge (期待画面を手書き) で試す
em = ["😀😃😄 smile", "👍 thumbs", "🎉✨🔥 party", "ab😀cd漢字ef", "x" * 90 + "😀😀", "ｱｲｳｴｵ ﾊﾝｶｸ ¥100 \\path"]
open(os.path.join(w, "emoji.txt"), "w", encoding="utf-8").write("\n".join(em) + "\n")
PY

# ---- tmux ----
if sel app_tmux; then
cat > "$W/tmux.conf" <<'EOF'
set -g status-left "[vt] "
set -g status-right "fixed-right"
set -g status-interval 0
set -g automatic-rename off
set -g default-command "/bin/bash --norc --noprofile -i"
set -g base-index 1
EOF
cat > "$W/tmux.script" <<'EOF'
wait 1500
send printf '\\e[31mred\\e[32m green\\e[0m 日本語 tmux\\n'\r
until 500
send seq 1 40\r
until 600
send \x02%
wait 1000
send printf 'right pane 右のペイン\\n'; ls /usr | head -5\r
until 700
send \x02"
wait 1000
send printf '\\e[1;33mbottom\\e[0m pane\\n'\r
until 600
send \x02o
wait 500
send echo back-to-left\r
until 600
EOF
env -i HOME="$W" TERM=xterm LANG=en_US.UTF-8 PATH=/opt/homebrew/bin:/usr/bin:/bin PS1='$ ' BASH_SILENCE_DEPRECATION_WARNING=1 \
  $REC fixtures/app_tmux.rec $COLS $ROWS xterm "$W/tmux.script" -- \
  tmux -S "$W/tmux.sock" -f "$W/tmux.conf" new-session -x $COLS -y $ROWS || true
tmux -S "$W/tmux.sock" kill-server 2>/dev/null || true
sizes_add app_tmux
fi

# ---- Claude Code ----
if sel app_claude_help; then
cat > "$W/help.script" <<'EOF'
until 1500
EOF
$REC fixtures/app_claude_help.rec $COLS $ROWS xterm "$W/help.script" -- claude --help || true
sizes_add app_claude_help
fi
if sel app_claude_trust || sel app_claude; then
# 作業ディレクトリは短い固定名の空ディレクトリ (画面にパスが出るので、個人のパスや折り返しを避ける)
# この記録を Claude Code の中から走らせたときの環境 (子セッション印・親との通信路) は引き継がない
for v in $(env | sed -n 's/^\(CLAUDE[A-Z_]*\)=.*/\1/p'); do unset "$v"; done
CW=$(mktemp -d /tmp/vtfx.XXXX); trap 'tmux -S "$W/tmux.sock" kill-server 2>/dev/null || true; rm -rf "$W" "$CW"' EXIT
# 起動直後の「このフォルダを信用するか」の画面 (API は呼ばない。端末への問い合わせ OSC 11/DA1/XTVERSION/kitty が入る)
cat > "$W/trust.script" <<'EOF'
wait 5000
until 1500
EOF
( cd "$CW" && $REC "$OLDPWD/fixtures/app_claude_trust.rec" $COLS $ROWS xterm "$W/trust.script" -- claude ) || true
sizes_add app_claude_trust
fi
if sel app_claude; then
# 信用して (↓ Enter) hello と答えさせ、/exit で抜ける。道具は使わせない
cat > "$W/claude.script" <<'EOF'
wait 5000
until 1500
key down
wait 500
send \r
wait 5000
until 1500
send Reply with just the single word hello. Do not use any tools.
wait 800
send \r
wait 15000
until 3000
send /exit
wait 800
send \r
until 2000
EOF
( cd "$CW" && $REC "$OLDPWD/fixtures/app_claude.rec" $COLS $ROWS xterm "$W/claude.script" -- \
    claude --disallowedTools "Bash Edit Write NotebookEdit WebFetch WebSearch" ) || true
# 終わり (/exit) で代替画面を抜けるので、pyte と比べられるよう最後の ?1049l から後を落とす (応答の出た画面が残る)
$PY -I -c 'import sys; p=sys.argv[1]; d=open(p,"rb").read(); i=d.rfind(b"\x1b[?1049l"); open(p,"wb").write(d[:i] if i>0 else d)' fixtures/app_claude.rec
sizes_add app_claude
fi

if sel app_claude_stream; then
# 長めの応答 (1〜80 の番号付きの行) を流させる。応答の途中で画面全体を描き直す様子が入る
for v in $(env | sed -n 's/^\(CLAUDE[A-Z_]*\)=.*/\1/p'); do unset "$v"; done
CW2=$(mktemp -d /tmp/vtfx.XXXX)
cat > "$W/stream.script" <<'EOF'
wait 5000
until 1500
key down
wait 500
send \r
wait 5000
until 1500
send Without using any tools, output the lines "line 1: 日本語の行" through "line 80: 日本語の行", one per line, nothing else.
wait 800
send \r
wait 40000
until 4000
EOF
( cd "$CW2" && $REC "$OLDPWD/fixtures/app_claude_stream.rec" $COLS $ROWS xterm "$W/stream.script" -- \
    claude --disallowedTools "Bash Edit Write NotebookEdit WebFetch WebSearch" ) || true
rm -rf "$CW2"
sizes_add app_claude_stream
fi

# ---- vim (日本語) ----
if sel app_vimjp; then
cat > "$W/vim.script" <<'EOF'
wait 800
send :set nonu noai enc=utf-8\r
wait 300
send 5G
wait 200
send 10l
wait 200
send i挿入した文字<>
wait 300
send \e
wait 200
send 20G
until 400
send dd
until 300
send :set nu\r
until 500
EOF
$REC fixtures/app_vimjp.rec $COLS $ROWS xterm "$W/vim.script" -- vim -u NONE -N "$W/jp.txt" || true
sizes_add app_vimjp
fi

# ---- top ----
if sel app_top; then
cat > "$W/top.script" <<'EOF'
wait 1500
until 1200
until 1200
EOF
$REC fixtures/app_top.rec $COLS $ROWS xterm "$W/top.script" -- top -n 30 || true
# top は記録の終わりに自分で代替画面を抜ける (pyte は代替画面を持たないので) 最後の ?1049l から後を落とす
$PY -I -c 'import sys; p=sys.argv[1]; d=open(p,"rb").read(); i=d.rfind(b"\x1b[?1049l"); open(p,"wb").write(d[:i] if i>0 else d)' fixtures/app_top.rec
sizes_add app_top
fi

# ---- less (UTF-8) ----
if sel app_lessu8; then
cat > "$W/less.script" <<'EOF'
wait 600
send j
wait 200
send f
until 400
send b
until 400
send /日本\r
until 500
EOF
LESSCHARSET=utf-8 $REC fixtures/app_lessu8.rec $COLS $ROWS xterm "$W/less.script" -- less "$W/u8.txt" || true
sizes_add app_lessu8
fi

# ---- git log --graph ----
if sel app_gitlog; then
cat > "$W/git.script" <<'EOF'
until 1500
EOF
( cd .. && $REC tests/fixtures/app_gitlog.rec $COLS $ROWS xterm "$W/git.script" -- \
    git --no-pager log --graph --color=always --format='%C(yellow)%h%C(reset) %C(green)%ad%C(reset) %s%C(auto)%d' \
    --date=short -n 40 dd8e297 ) || true
sizes_add app_gitlog
fi

# ---- 絵文字・境界 ----
if sel app_emoji; then
cat > "$W/emoji.script" <<EOF
send cat "$W/emoji.txt"\r
until 800
EOF
env -i HOME="$W" TERM=xterm LANG=en_US.UTF-8 PATH=/usr/bin:/bin PS1='$ ' BASH_SILENCE_DEPRECATION_WARNING=1 \
  $REC fixtures/app_emoji.rec $COLS $ROWS xterm "$W/emoji.script" -- /bin/bash --norc --noprofile -i || true
sizes_add app_emoji
fi

# ---- 個人情報を消す (同じバイト長で置き換え) ----
for f in fixtures/app_*.rec; do
  $PY -I scrub.py "$f" "$W" "$(id -un)" "$(hostname -s)" "$HOME" "$(git config user.email || true)" "$(git config user.name || true)" "Claude Max"
done
ls -la fixtures/app_*.rec
