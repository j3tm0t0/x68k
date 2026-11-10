#!/usr/bin/env bash
# npd.x -s (SCSI IOCS で ID 6 と交換する、本物の X68000 と BlueSCSI 用) を XEiJ + picosim で試す。
#   picosim のメールボックスの模倣を切り (picosim.mailbox=off)、ID 6 を XEiJ の SPC (IOCS _SCSIDRV) の経路で picosim に渡す。
#   AUTOEXEC.BAT が npd.x -s を上げ、acceltest.x (RFC 7748 / 8032、net/accel) を回して結果をホストのディレクトリに書く。
# 要るもの: XEiJ と agent (x68kcli/tools)、build/picosim。GUI のセッションで動かす
# 使い方: harness/picosim/tests/accelscsi.sh     環境変数: ACCEL_OUT (既定 $TMPDIR/accelscsi)、NOXEIJ=1 で飛ばす
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
if [ -n "${NOXEIJ:-}" ]; then echo "SKIP accelscsi (NOXEIJ)"; exit 0; fi
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
AGENT=$TOP/x68kcli/tools/xeijagent.jar
if [ ! -f "$XEIJ/XEiJ.jar" ] || [ ! -f "$AGENT" ]; then
  echo "SKIP accelscsi: no XEiJ / agent in x68kcli/tools (make -C x68kcli tools agent)"; exit 0; fi
[ -x build/picosim ] || { echo "SKIP accelscsi: no build/picosim (make -C harness/picosim)"; exit 0; }
. tests/benchlock.sh
benchlock_take accelscsi || { echo "SKIP accelscsi: bench locked by $(cat "$XEIJLOCK")"; exit 0; }
OUT=${ACCEL_OUT:-${TMPDIR:-/tmp}/accelscsi}
BRIDGE=${BRIDGE:-26969}
B="$OUT/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
make -s -C "$TOP/net/npd" || exit 1
make -s -C "$TOP/net/accel" acceltest.x || exit 1

rm -rf "$B" "$OUT/tmp" "$OUT/mem.bin"; mkdir -p "$B" "$OUT/tmp"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp "$TOP/net/npd/npd.sys" "$B/NPD.SYS"; cp "$TOP/net/npd/npd.x" "$B/NPD.X"
cp "$TOP/net/accel/acceltest.x" "$B/ACCTEST.X"
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nPROCESS = 16 10 19\r\nSHELL = A:\\COMMAND.X\r\nDEVICE = \\NPD.SYS\r\n' > "$B/CONFIG.SYS"
# picosim は見出しの stat (Z の IP) が 0 の交換を捨てる (ファームの UDP モード) ので IP を付ける。BlueSCSI は stat を見ない。
# NPD_ARGS=192.168.1.68 PICOSIM_MAILBOX=on で Z のメールボックスの経路 (比べる用)
printf 'ECHO OFF\r\nPATH A:\\\r\nNPD.X %s > NPD.TXT\r\nACCTEST.X -n 3 > ACC.TXT\r\nCOPY CONFIG.SYS DONE.TXT\r\n' "${NPD_ARGS:--s 192.168.1.68}" > "$B/AUTOEXEC.BAT"

pids=()
cleanup() { for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null || true; done; benchlock_release; }
trap cleanup EXIT
# -c 0: BlueSCSI には Z の読み込みキャッシュが無い
build/picosim ${PICOSIM_ARGS:--c 0} -l 127.0.0.1:$BRIDGE -s 10 > "$OUT/picosim.log" 2>&1 & pids+=($!)
sleep 1
"$JAVA" -javaagent:"$AGENT=$OUT/tmp/agent.cmd" -Dpicosim=127.0.0.1:$BRIDGE -Dpicosim.ids=6 -Dpicosim.mailbox=${PICOSIM_MAILBOX:-off} -Dpicosim.log=${PICOSIM_LOG:-0} \
  -Djava.io.tmpdir="$OUT/tmp" \
  -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off \
  > "$OUT/xeij.log" 2>&1 & pids+=($!)
XPID=$!
rc=1
for t in $(seq 1 "${ACCEL_TIMEOUT:-300}"); do
  [ -f "$B/DONE.TXT" ] && { rc=0; break; }
  kill -0 $XPID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij.log)"; break; }
  sleep 1
done
[ $rc -eq 0 ] || echo "時間切れか XEiJ の終了 (ログ: $OUT)"
# npd.x の数 ("SLPDZSTA" xchg retry err drops / "SLPDZERR" wr rd stale len rd2) をメモリから読む
echo "dump $OUT/mem.bin 0 0xc00000" > "$OUT/tmp/agent.cmd"
for t in 1 2 3 4 5 6 7 8 9 10; do [ -f "$OUT/mem.bin" ] && break; sleep 1; done
[ -f "$OUT/mem.bin" ] && python3 -I -c '
import struct, sys
m = open(sys.argv[1], "rb").read()
for tag, names in ((b"SLPDSTAT", "loops rx tx bad"), (b"SLPDZSTA", "xchg retry err drops"), (b"SLPDZERR", "wr rd stale len rd2")):
    i = m.find(tag)
    if i >= 0:
        v = struct.unpack(">%dI" % len(names.split()), m[i + 8:i + 8 + 4 * len(names.split())])
        print("npd", tag.decode(), " ".join("%s=%d" % kv for kv in zip(names.split(), v)))
' "$OUT/mem.bin"
for f in NPD ACC; do
  [ -f "$B/$f.TXT" ] && { echo "== $f.TXT"; iconv -f CP932 -t UTF-8 "$B/$f.TXT" 2>/dev/null | tr -d '\r'; }
done
grep -E "^(picosim|netpipe|accel)" "$OUT/picosim.log" | tail -5
fails=0
grep -q "BlueSCSI, SCSI ID 6, SCSI IOCS" "$B/NPD.TXT" 2>/dev/null && echo "PASS npd.x -s started" || { echo "FAIL npd.x -s started"; fails=$((fails + 1)); }
if [ -f "$B/ACC.TXT" ] && grep -q OK "$B/ACC.TXT" && ! grep -q FAIL "$B/ACC.TXT"; then echo "PASS acceltest.x over npd.x -s"
else echo "FAIL acceltest.x over npd.x -s"; fails=$((fails + 1)); fi
echo "$fails failure(s)"
exit $((fails > 0))
