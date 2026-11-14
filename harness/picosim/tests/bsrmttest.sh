#!/usr/bin/env bash
# bsrmt.sys (BlueSCSI の SD カードのリモートドライブ、net/bsrmt、x68k-zos #188) を XEiJ + picosim -X で試す。
#   XEiJ はホストのディレクトリから起動し (A:)、CONFIG.SYS で BSRMT.SYS を組み込む。picosim -X の中身 (Mac の
#   ディレクトリ) を、AUTOEXEC.BAT の DIR / TYPE / COPY / MKDIR / REN / DEL で読み書きし、結果を Mac 側で確かめる。
# 要るもの: XEiJ と agent (x68kcli/tools、無ければ x68k の本体の作業ディレクトリのもの)、remoteserv.c (picosim の B:)。
# 無ければ SKIP。NOXEIJ=1 でも飛ばす
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
MAIN=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
skip() { echo "SKIP bsrmttest: $*"; exit 0; }
[ -n "${NOXEIJ:-}" ] && skip "NOXEIJ"
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}; AGENT=${AGENT:-$TOP/x68kcli/tools/xeijagent.jar}
[ -f "$XEIJ/XEiJ.jar" ] || XEIJ=$MAIN/x68kcli/tools/xeij
[ -f "$AGENT" ] || AGENT=$MAIN/x68kcli/tools/xeijagent.jar
[ -f "$XEIJ/XEiJ.jar" ] && [ -f "$AGENT" ] || skip "no XEiJ / agent in x68kcli/tools"
# PICOSIM=: another picosim with the same -X (for example one with the BlueSCSI firmware's server, which writes
# the card back to the -X directory when it gets SIGTERM)
PICOSIM=${PICOSIM:-build/picosim}
[ "$(nm "$PICOSIM" 2>/dev/null | grep -c -e remote_serv -e x68rd_port)" -gt 0 ] || skip "$PICOSIM has no remoteserv.c (RS=)"
make -s -C "$TOP/net/bsrmt" || { echo "FAIL build bsrmt.sys"; exit 1; }
. "$(dirname "$0")/benchlock.sh"
benchlock_take bsrmttest || skip "bench locked by $(cat "$XEIJLOCK")"
WORK=${WORK:-${TMPDIR:-/tmp}/picosim-bsrmttest}
BRIDGE=${BRIDGE:-26971}
B=$WORK/boot; R=$WORK/sd
rm -rf "$WORK"; mkdir -p "$B" "$R/MANY" "$R/SUB/DEEP" "$WORK/tmp"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
pids=()
cleanup() { for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done; benchlock_release; }
trap cleanup EXIT

# ---- SD カードの中身 (Mac のディレクトリ)
printf 'hello from the sd card\r\n' > "$R/HELLO.TXT"
head -c 200000 /dev/urandom > "$R/BIG.BIN"            # READ を 15872 バイトずつ 13 回
for i in $(seq 1 50); do printf 'file %d\r\n' "$i" > "$R/MANY/F$i.TXT"; done   # FILES を複数件ずつ
printf 'deep\r\n' > "$R/SUB/DEEP/D.TXT"
printf 'old\r\n' > "$R/OLD.TXT"
printf 'gone\r\n' > "$R/GONE.TXT"
printf 'nihongo\r\n' > "$R/日本語.TXT"                     # 名前は UTF-8 (カードの上) と Shift_JIS (X68000) で変わる

# ---- 起動ディスク
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp "$TOP/net/bsrmt/bsrmt.sys" "$B/BSRMT.SYS"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nSHELL = A:\\COMMAND.X\r\nDEVICE = \\BSRMT.SYS\r\n' > "$B/CONFIG.SYS"
# XEiJ (SUPER) をホストのディレクトリから起動すると A: がそこ、B: と C: がフロッピー、組み込んだドライブは D:
D=${DRV:-D:}
{
  printf 'ECHO OFF\r\nPATH A:\\\r\n'
  printf 'DIR %s\\ > DIR.TXT\r\n' "$D"
  printf 'TYPE %s\\HELLO.TXT > TYPE.TXT\r\n' "$D"
  printf 'COPY %s\\BIG.BIN A:\\BIG.BIN > NUL\r\n' "$D"
  printf 'COPY A:\\BIG.BIN %s\\BIG2.BIN > NUL\r\n' "$D"
  printf 'DIR %s\\MANY > MANY.TXT\r\n' "$D"
  printf 'TYPE %s\\SUB\\DEEP\\D.TXT > DEEP.TXT\r\n' "$D"
  printf 'MKDIR %s\\NEWDIR\r\nCOPY CONFIG.SYS %s\\NEWDIR\\C.SYS > NUL\r\n' "$D" "$D"
  printf 'REN %s\\OLD.TXT NEW.TXT\r\nDEL %s\\GONE.TXT\r\n' "$D" "$D"
  printf 'TYPE %s\\日本語.TXT > JP.TXT\r\nCOPY CONFIG.SYS %s\\漢字.SYS > NUL\r\n' "$D" "$D"
  printf 'COPY CONFIG.SYS DONE.TXT > NUL\r\n'
} | iconv -f UTF-8 -t CP932 > "$B/AUTOEXEC.BAT"

"$PICOSIM" -l 127.0.0.1:$BRIDGE -L none -X "$R" -vv > "$WORK/picosim.log" 2>&1 & pids+=($!)
PSPID=$!
sleep 1
"$JAVA" -javaagent:"$AGENT=$WORK/tmp/agent.cmd" -Dpicosim=127.0.0.1:$BRIDGE -Dpicosim.ids=6 -Djava.io.tmpdir="$WORK/tmp" \
  -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off \
  > "$WORK/xeij.log" 2>&1 & pids+=($!)
XPID=$!
for t in $(seq 1 ${TIMEOUT:-180}); do
  [ -f "$B/DONE.TXT" ] && break
  kill -0 $XPID 2>/dev/null || { echo "FAIL XEiJ stopped (log $WORK/xeij.log)"; exit 1; }
  sleep 1
done
sleep 1
kill -TERM $PSPID 2>/dev/null; wait $PSPID 2>/dev/null
fails=0
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
txt() { iconv -f CP932 -t UTF-8 "$B/$1" 2>/dev/null | tr -d '\r'; }
ok "the boot finished" '[ -f "$B/DONE.TXT" ]'
ok "DIR of the root lists HELLO.TXT and the dirs" 'txt DIR.TXT | grep -q "HELLO" && txt DIR.TXT | grep -q "MANY"'
ok "TYPE reads a small file" 'txt TYPE.TXT | grep -q "hello from the sd card"'
ok "COPY from the card (200000 bytes)" 'cmp -s "$R/BIG.BIN" "$B/BIG.BIN"'
ok "COPY to the card (200000 bytes)" 'cmp -s "$R/BIG.BIN" "$R/BIG2.BIN"'
n=$(txt MANY.TXT | grep -c "^F[0-9]")
ok "DIR of 50 files lists all ($n)" '[ "$n" = 50 ]'
ok "TYPE in a subdirectory" 'txt DEEP.TXT | grep -q deep'
ok "MKDIR and COPY into it" 'cmp -s "$B/CONFIG.SYS" "$R/NEWDIR/C.SYS"'
ok "REN" '[ -f "$R/NEW.TXT" ] && [ ! -f "$R/OLD.TXT" ]'
ok "DEL" '[ ! -f "$R/GONE.TXT" ]'
ok "TYPE of a Japanese name" 'txt JP.TXT | grep -q nihongo'
ok "COPY to a Japanese name" 'cmp -s "$B/CONFIG.SYS" "$R/漢字.SYS"'
grep -q "x68rd: seq" "$WORK/picosim.log" && echo "(retries seen in $WORK/picosim.log)"
echo "$fails failure(s)"
exit $((fails > 0))
