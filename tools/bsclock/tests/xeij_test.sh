#!/usr/bin/env bash
# bsclock.x の試験: XEiJ (X68000 SUPER) + picosim。picosim の SCSI ID 6 (netpipe) が BlueSCSI の x68k-ota のファームの代わりに
# 0xA8 TIME に答える (PICOSIM_TIME で固定した時刻、PICOSIM_TZ の時差)。
#   1 回目: 2030-01-02 03:04:05 UTC (+9 で 12:04:05、水曜) を設定し、IOCS と Human68k の DOS で読み戻す。ID 5 (装置なし) は終了コード 1
#   2 回目: PICOSIM_NOTIME=1 (まだ時刻が無い) で終了コード 2、時計は変えない
# XEiJ を起動する道具 (xeij_run.sh) は j3tm0t0/x68k-zos の kernel/tests (ZOS_TESTS=、読むだけ)。無ければ SKIP。NOXEIJ=1 でも飛ばす
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$(cd ../.. && pwd)
SRC=$HOME/Dropbox/src/github.com/j3tm0t0
if [ -z "${ZOS_TESTS:-}" ]; then
  for d in "$SRC/x68k-zos-kernel/kernel/tests" "$SRC/x68k-zos/kernel/tests" "$SRC"/x68k-zos/.claude/worktrees/*/kernel/tests; do
    [ -f "$d/xeij_run.sh" ] && { ZOS_TESTS=$d; break; }
  done
fi
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
[ -f "$XEIJ/XEiJ.jar" ] || XEIJ=$SRC/x68k/x68kcli/tools/xeij
AGENT=${AGENT:-$XEIJ/../xeijagent.jar}
PICOSIM=${PICOSIM:-$TOP/harness/picosim/build/picosim}
WORK=${WORK:-${TMPDIR:-/tmp}/bsclock-xeij}
PICOPORT=${PICOPORT:-26871}
skip() { echo "SKIP bsclock xeij_test: $*"; exit 0; }
[ -n "${NOXEIJ:-}" ] && skip "NOXEIJ"
[ -f "${ZOS_TESTS:-}/xeij_run.sh" ] || skip "no x68k-zos kernel/tests/xeij_run.sh (ZOS_TESTS=)"
[ -f "$XEIJ/XEiJ.jar" ] && [ -f "$AGENT" ] || skip "no XEiJ / agent ($XEIJ)"
[ -x "$PICOSIM" ] || skip "no picosim ($PICOSIM; make -C harness/picosim)"
make -s bsclock.x || { echo "FAIL build"; exit 1; }
. "$TOP/harness/picosim/tests/benchlock.sh"
benchlock_take bsclock || skip "bench locked by $(cat "$XEIJLOCK")"
PPID_=
trap 'kill $PPID_ 2>/dev/null; benchlock_release' EXIT
rm -rf "$WORK" && mkdir -p "$WORK/t" && cp "$ZOS_TESTS"/{xeij_run.sh,xeijkeys.py} "$WORK/t/"

run() {   # run 名前 環境変数... -- AUTOEXEC の行...
  local name=$1; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  env "${envs[@]}" "$PICOSIM" -l 127.0.0.1:$PICOPORT -L none > "$WORK/picosim-$name.log" 2>&1 &
  PPID_=$!
  sleep 1
  AGENTJAR=$AGENT XEIJ=$XEIJ ZOS_OUT="$WORK/$name" ZOS_JAVA_OPTS="-Dpicosim=127.0.0.1:$PICOPORT -Dpicosim.ids=6" \
    FILES="$PWD/bsclock.x" ZOS_PROCESS= ZOS_TIMEOUT=${ZOS_TIMEOUT:-180} "$WORK/t/xeij_run.sh" "$@" > "$WORK/run-$name.log" 2>&1 || true
  kill $PPID_ 2>/dev/null; wait $PPID_ 2>/dev/null; PPID_=
}

run set PICOSIM_TIME=1893553445 PICOSIM_TZ=+9 -- \
  'BSCLOCK -r > R0.TXT' 'BSCLOCK -n > N.TXT' 'IF ERRORLEVEL 1 ECHO bad > NERR.TXT' 'BSCLOCK -r > R1.TXT' \
  'SET BSCLOCK_DEBUG=1' 'BSCLOCK > S.TXT' 'IF ERRORLEVEL 1 ECHO bad > SERR.TXT' 'BSCLOCK -r > R2.TXT' \
  'BSCLOCK -i 5 > E5.TXT' 'IF ERRORLEVEL 1 ECHO one > E5ERR.TXT' 'IF ERRORLEVEL 2 ECHO two > E5ERR2.TXT'
run notime PICOSIM_NOTIME=1 -- \
  'BSCLOCK -r > R0.TXT' 'BSCLOCK > S.TXT' 'IF ERRORLEVEL 2 ECHO two > SERR2.TXT' 'IF ERRORLEVEL 3 ECHO three > SERR3.TXT' \
  'BSCLOCK -r > R1.TXT'

fails=0
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
S=$WORK/set/boot N=$WORK/notime/boot
t() { tr -d '\r' < "$1" 2>/dev/null; }
ok "ran to the end (set)"                     "[ -f $S/DONE.TXT ]"
ok "-n shows the BlueSCSI time"               "[ \"\$(t $S/N.TXT)\" = 'BlueSCSI: 2030-01-02 12:04:05 Wed (UTC+9:00)' ] && [ ! -f $S/NERR.TXT ]"
ok "-n leaves the clock alone"                "! grep -q '2030-01-02' $S/R1.TXT"
ok "set: exit code 0"                         "[ ! -f $S/SERR.TXT ] && grep -q '^X68000: *2030-01-02 12:04:0[5-9] Wed (DOS 2030-01-02 Wed)' $S/S.TXT"
ok "set: read back by IOCS and DOS"           "grep -q '^2030-01-02 12:0[45]:[0-9][0-9] Wed (DOS 2030-01-02 Wed)' $S/R2.TXT"
ok "BCD from _DATEBCD / _TIMEBCD"            "grep -q 'debug: date 23500102 time ..120405' $S/S.TXT"
ok "no netpipe on ID 5: exit code 1"          "[ -f $S/E5ERR.TXT ] && [ ! -f $S/E5ERR2.TXT ]"
ok "ran to the end (notime)"                  "[ -f $N/DONE.TXT ]"
ok "no time yet: exit code 2"                 "[ -f $N/SERR2.TXT ] && [ ! -f $N/SERR3.TXT ] && [ ! -s $N/S.TXT ]"
ok "no time yet: clock unchanged"             "! grep -q '2030-01-02' $N/R1.TXT"
ok "picosim answered TIME"                    "grep -q 'bootctl: TIME 2030-01-02 12:04:05' $WORK/picosim-set.log"
for f in R0 N S R2 E5; do echo "     set/$f: $(t $S/$f.TXT | tr '\n' '|')"; done
echo "     notime/R0: $(t $N/R0.TXT)  notime/R1: $(t $N/R1.TXT)"
echo "$fails failure(s)"
[ $fails = 0 ]
