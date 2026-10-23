# benchlock.sh -- XEiJ + picosim の試験台の排他 (zlock と同じ決まり)。tests/tsptest.sh と zostest.sh が source する
#   ファイル $XEIJLOCK (既定 scratchpad/xeijlock、無ければ $TMPDIR/xeijlock) に持ち主の名前を書く。
#   無いか自分の名前のときだけ取る (noclobber で作るので同時に取れない)。外すときは消す (ほかの人のものは上書きしない)
#   名前は XEIJLOCK_NAME (既定 picosim-$USER)。持たれていたら XEIJLOCK_WAIT 秒 (既定 900) 待ち、だめなら SKIP する
SCR=/private/tmp/claude-501/-Users-moto-Dropbox-src-github-com-j3tm0t0-x68kcli/62eb2a4a-02b9-4915-890f-ce59a34745bb/scratchpad
XEIJLOCK=${XEIJLOCK:-$([ -d "$SCR" ] && echo "$SCR/xeijlock" || echo "${TMPDIR:-/tmp}/xeijlock")}
XEIJLOCK_NAME=${XEIJLOCK_NAME:-picosim-$USER}
benchlock_take() {
  local t=0 who
  while :; do
    if (set -o noclobber; echo "$XEIJLOCK_NAME $(date +%H:%M:%S) $1" > "$XEIJLOCK") 2>/dev/null; then BENCHLOCK_MINE=1; return 0; fi
    who=$(cat "$XEIJLOCK" 2>/dev/null)
    case "$who" in "$XEIJLOCK_NAME "*) BENCHLOCK_MINE=; return 0;; esac   # 自分のもの (外の呼び手が取っている)
    [ $t -eq 0 ] && echo "     bench busy ($XEIJLOCK: $who), waiting"
    t=$((t + 5)); [ $t -ge "${XEIJLOCK_WAIT:-900}" ] && return 1
    sleep 5
  done
}
benchlock_release() {
  [ -n "${BENCHLOCK_MINE:-}" ] || return 0
  case "$(cat "$XEIJLOCK" 2>/dev/null)" in "$XEIJLOCK_NAME "*) rm -f "$XEIJLOCK";; esac
}
