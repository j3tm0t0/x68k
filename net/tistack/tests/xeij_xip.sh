#!/usr/bin/env bash
# 比べる用: 同じ XEiJ + picosim で xip.x の道 (npd.x の IP レコード、xip.x が TCP を話す) の速さを測る。
#   HDS=... (x68kcli で make image NET=npd NETZIP=192.168.1.68 したもの。起動すると netup → npd.x → rcmd -d)
#   Mac → Z: x68put (圧縮しない 300KB)、Z → Mac / Mac → Z: tisnet.x sink / src (x68put で送ってから rcmd で動かす)
# picosim の lwIP (tsp_svc) は Z の IP を名乗らないよう TSP_IP=192.168.1.118 にする
set -euo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
AGENT=$TOP/x68kcli/tools/xeijagent.jar
OUT=${TIS_OUT:-${TMPDIR:-/tmp}/tisxip}
HDS=${HDS:?HDS=npd 入りのイメージ}
PEERPORT=${PEERPORT:-7787} BRIDGE=${BRIDGE:-26968} RPORT=${RPORT:-23468}
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
mkdir -p "$OUT/tmp"
cp "$HDS" "$OUT/z.hds"
make -s -C tests tisnet.x
pids=()
trap 'for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done' EXIT
python3 -I tests/tisnet_peer.py "$PEERPORT" > "$OUT/peer.log" 2>&1 & pids+=($!)
TSP_IP=192.168.1.118 "$TOP/harness/picosim/build/picosim" -l 127.0.0.1:$BRIDGE -L $RPORT:2323 -s 10 > "$OUT/picosim.log" 2>&1 & pids+=($!)
sleep 1
"$JAVA" -javaagent:"$AGENT=$OUT/tmp/agent.cmd" -Dpicosim=127.0.0.1:$BRIDGE -Dpicosim.ids=6 -Djava.io.tmpdir="$OUT/tmp" \
  -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER -boot=sc0 -si0="$OUT/z.hds" -flrinformed=on -usecanvas=off \
  > "$OUT/xeij.log" 2>&1 & pids+=($!)
export X68_HOST=127.0.0.1 X68_PORT=$RPORT
for t in $(seq 1 ${TIS_TIMEOUT:-300}); do
  "$TOP/harness/raspi/x68.sh" 'echo up' >/dev/null 2>&1 && break
  sleep 2
done
echo "rcmd up after ~$((t*2)) s"
head -c 300000 /dev/urandom > "$OUT/put.bin"
X68PUT_RAW=1 "$TOP/harness/raspi/x68put.sh" "$OUT/put.bin" 'A:/put.bin'
X68PUT_RAW=1 "$TOP/harness/raspi/x68put.sh" tests/tisnet.x 'A:/tisnet.x'
"$TOP/harness/raspi/x68.sh" "A:/tisnet.x echo 192.168.1.1 $PEERPORT 20"
"$TOP/harness/raspi/x68.sh" "A:/tisnet.x sink 192.168.1.1 $PEERPORT ${RAWSIZE:-1000000}"
"$TOP/harness/raspi/x68.sh" "A:/tisnet.x src 192.168.1.1 $PEERPORT ${RAWSIZE:-1000000}"
"$TOP/harness/raspi/x68.sh" 'exit' >/dev/null 2>&1 || true
