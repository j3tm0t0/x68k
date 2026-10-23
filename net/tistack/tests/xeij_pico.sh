#!/usr/bin/env bash
# tis.x -p を XEiJ で、本物の lwIP (harness/picosim に載せた tsp_svc) 越しに試す。Z も Pico も要らない。
#
#   XEiJ (npd.sys 1.2 + tis.x -p + npd.x) ─メールボックス─> picosim (netpipe + tsp_svc + lwIP 192.168.1.68)
#       ─libslirp─> Mac (ゲートウェイ 192.168.1.1 = Mac の 127.0.0.1)
#
#   1. Mac の 127.0.0.1:7787 に相手 (tisnet_peer.py)、picosim を 127.0.0.1:26968 で -L 23468:2323 付きで起動
#   2. ホストのディレクトリから XEiJ を起動。AUTOEXEC.BAT が tis.x -p・npd.x を上げ、tisnet.x (名前・put・get・echo)、
#      ssh.x (Mac の sshd、鍵交換まで) を動かして DONE1.TXT を作り、最後に rcmd.x を立てる
#   3. Mac から 127.0.0.1:23468 → (slirp) → lwIP の 2323 → tis.x → rcmd.x: echo、x68put (300KB)、exit
#   4. rcmd.x が終わると tis.x -s を書いて DONE.TXT
# 使い方: tests/xeij_pico.sh     環境変数: TIS_OUT (既定 $TMPDIR/tispico)、SSH_X、RCMD_X、PUTSIZE、GETSIZE、NET=xip (下)
# NET=xip: 同じ XEiJ / picosim で xip.x の道 (比べる用)。tis.x の代わりに XIP_X / ifconfig 類を使う (XIPDIR)
set -euo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
AGENT=$TOP/x68kcli/tools/xeijagent.jar
OUT=${TIS_OUT:-${TMPDIR:-/tmp}/tispico}
RCMD_X=${RCMD_X:-${TMPDIR:-/tmp}/tistest/rcmd.x}
SSH_X=${SSH_X:-$HOME/Dropbox/src/github.com/j3tm0t0/x68k-ssh/net/ssh/dropbear/ssh.x}
PUTSIZE=${PUTSIZE:-300000}
GETSIZE=${GETSIZE:-300000}
PEERPORT=${PEERPORT:-7787}
# ほかの picosim / XEiJ (6868, 23268) とぶつからない口
BRIDGE=${BRIDGE:-26968}
RPORT=${RPORT:-23468}
B="$OUT/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
[ -f "$AGENT" ] || { echo "エージェントが無い: (cd x68kcli && make agent)"; exit 1; }
make -s
make -s -C ../npd
make -s -C tests tisnet.x tisrun.x
[ -n "${NOBUILD:-}" ] || make -s -C pico/picosim

rm -rf "$B" "$OUT/tmp"; mkdir -p "$B/TMP" "$OUT/tmp"
[ -d "$B/tmp" ] || mkdir "$B/tmp"     # rcmd.x は A:\tmp に書く (Linux では大文字小文字を区別するので TMP とは別)
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp tis.x "$B/TIS.X"; cp ../npd/npd.sys "$B/NPD.SYS"; cp ../npd/npd.x "$B/NPD.X"
cp tests/tisnet.x "$B/TISNET.X"; cp tests/tisrun.x "$B/TISRUN.X"
# SHELL は明示する (XEiJ のホストのディレクトリの起動は、Linux (Docker) では名前の大文字小文字を区別し、Human68k の既定の
# "COMMAND.x" では COMMAND.X が見つからなかった)
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nPROCESS = 16 10 20\r\nSHELL = A:\\COMMAND.X\r\nDEVICE = \\NPD.SYS\r\n' > "$B/CONFIG.SYS"
{
  printf 'ECHO OFF\r\nPATH A:\\\r\n'
  printf 'TIS.X -p > TIS.TXT\r\nNPD.X 192.168.1.68 > NPD.TXT\r\n'
  printf 'TISNET.X resolve mac.test %s > RES.TXT\r\n' "${RESOLVE:-example.com}"
  if [ -n "${TSP_OFF:-}" ]; then
    # TSP を知らない Pico の真似: 名前も接続も固まらずに失敗し、tis.x は残る
    printf 'TISNET.X echo 192.168.1.1 %d 1 > ECHO.TXT\r\nTIS.X -s > STAT.TXT\r\nCOPY CONFIG.SYS DONE1.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n' "$PEERPORT"
  fi
  printf 'TISNET.X echo 192.168.1.1 %d 20 > ECHO.TXT\r\n' "$PEERPORT"
  printf 'TISNET.X put 192.168.1.1 %d %d > PUT.TXT\r\n' "$PEERPORT" "$PUTSIZE"
  printf 'TISNET.X get mac.test %d %d > GET.TXT\r\n' "$PEERPORT" "$GETSIZE"
  printf 'TISNET.X sink 192.168.1.1 %d %d > SINK.TXT\r\n' "$PEERPORT" "${RAWSIZE:-1000000}"
  printf 'TISNET.X src 192.168.1.1 %d %d > SRC.TXT\r\n' "$PEERPORT" "${RAWSIZE:-1000000}"
  if [ -f "$SSH_X" ]; then
    cp "$SSH_X" "$B/SSH.X"
    # stderr を取るために tisrun.x を通す (tisrun の偽サーバは 127.0.0.1:22 で使われない)
    printf 'TISRUN.X -o SSHRES.TXT -e SSHERR.TXT ssh SSH.X -y -y nobody@192.168.1.1 true > SSH.TXT\r\n'
  fi
  printf 'COPY CONFIG.SYS DONE1.TXT\r\n'
  if [ -f "$RCMD_X" ]; then
    cp "$RCMD_X" "$B/RCMD.X"
    printf 'RCMD.X -s A:\\COMMAND.X -d > RCMD.TXT\r\n'
  fi
  printf 'TIS.X -s > STAT.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"

pids=()
cleanup() { for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
# Pico (lwIP) の DNS: picosim の DNS の口 (picosim が Mac の resolver で答える。bacb837 から lan_attach の口にも)
export TSP_DNS=${TSP_DNS:-192.168.1.3}
python3 -I tests/tisnet_peer.py "$PEERPORT" > "$OUT/peer.log" 2>&1 & pids+=($!)
"$TOP/harness/picosim/build/picosim" -l 127.0.0.1:$BRIDGE -L $RPORT:2323 -s 10 > "$OUT/picosim.log" 2>&1 & pids+=($!)
sleep 1
"$JAVA" -javaagent:"$AGENT=$OUT/tmp/agent.cmd" -Dpicosim=127.0.0.1:$BRIDGE -Dpicosim.ids=6 -Djava.io.tmpdir="$OUT/tmp" \
  -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off \
  > "$OUT/xeij.log" 2>&1 & pids+=($!)
XPID=$!
wait_for() {
  for t in $(seq 1 "$2"); do
    [ -f "$B/$1" ] && return 0
    kill -0 $XPID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij.log)"; return 1; }
    sleep 1
  done
  echo "時間切れ ($1)"; return 1
}
wait_for DONE1.TXT ${TIS_TIMEOUT:-600}
show() { for f in "$@"; do [ -f "$B/$f.TXT" ] && { echo "== $f.TXT"; iconv -f CP932 -t UTF-8 "$B/$f.TXT" 2>/dev/null | tr -d '\r'; }; done; return 0; }
show TIS NPD RES ECHO PUT GET SINK SRC SSH SSHRES SSHERR
if [ -f "$RCMD_X" ]; then
  export X68_HOST=127.0.0.1 X68_PORT=$RPORT
  sleep 3
  echo "== rcmd (Mac -> 127.0.0.1:$RPORT -> slirp -> lwIP 192.168.1.68:2323 -> tis.x -> rcmd.x)"
  if "$TOP/harness/raspi/x68.sh" 'echo hello from tis' ; then echo "PASS rcmd: echo rc=0"; else echo "FAIL rcmd: echo"; fi
  head -c "$PUTSIZE" /dev/urandom > "$OUT/put.bin"
  t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
  if X68PUT_RAW=1 "$TOP/harness/raspi/x68put.sh" "$OUT/put.bin" 'A:\PUT.BIN'; then r=ok; else r=fail; fi
  t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
  "$TOP/harness/raspi/x68.sh" exit >/dev/null 2>&1 || true
  wait_for DONE.TXT 60 || true
  if cmp -s "$OUT/put.bin" "$B/PUT.BIN"; then echo "PASS rcmd: x68put $PUTSIZE bytes ($r) $(perl -e "printf '%.1f', $PUTSIZE/1024/($t1-$t0)") KB/s, content matches"
  else echo "FAIL rcmd: x68put ($r), content differs"; fi
fi
show RCMD STAT
grep -E "^(picosim|netpipe|lan):" "$OUT/picosim.log" | tail -3
