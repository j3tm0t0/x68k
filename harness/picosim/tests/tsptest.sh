#!/usr/bin/env bash
# tis.x -p (Z の TCP/IP を Pico に任せる) を XEiJ で端から端まで: net/tistack/tests/xeij_pico.sh を回して結果を判定する。
#   picosim (TSP 入りの build/picosim) ─ XEiJ (npd.sys + tis.x -p + npd.x) で、名前解決・echo・put・get・sink・src・
#   ssh.x (Mac の sshd に鍵交換まで)・Mac からの rcmd.x (echo と x68put 300KB)
# 要るもの: lwIP (LWIP=、TSP 入りのビルド)、XEiJ と agent (x68kcli/tools)、GUI のセッション。無ければ SKIP
# NOXEIJ=1 で飛ばす。名前は外に聞かない (RESOLVE=localhost)
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
if [ -n "${NOXEIJ:-}" ]; then echo "SKIP tsptest (NOXEIJ)"; exit 0; fi
if ! nm build/picosim 2>/dev/null | grep -q "tsp\|lwip" && ! strings build/picosim | grep -q "socket service for tis.x"; then
  echo "SKIP tsptest: build/picosim has no TSP (lwIP not found; make LWIP=...)"; exit 0; fi
if [ ! -f "$TOP/x68kcli/tools/xeij/XEiJ.jar" ] || [ ! -f "$TOP/x68kcli/tools/xeijagent.jar" ]; then
  echo "SKIP tsptest: no XEiJ / agent in x68kcli/tools (make -C x68kcli tools agent)"; exit 0; fi
OUT=${TIS_OUT:-${TMPDIR:-/tmp}/picosim-tsptest}
log=$OUT.log
mkdir -p "$(dirname "$log")"
RESOLVE=${RESOLVE:-localhost} TIS_OUT=$OUT NOBUILD=1 timeout 900 "$TOP/net/tistack/tests/xeij_pico.sh" > "$log" 2>&1
rc=$?
fails=0
ok() { if grep -q -- "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
B=$OUT/boot
for f in RES ECHO PUT GET SINK SRC; do
  if [ -f "$B/$f.TXT" ] && ! grep -q FAIL "$B/$f.TXT" && grep -q PASS "$B/$f.TXT"; then echo "PASS tis.x -p: $f"
  else echo "FAIL tis.x -p: $f"; fails=$((fails + 1)); fi
done
if [ -f "$B/SSHERR.TXT" ]; then ok "ssh.x to the Mac's sshd over TSP (key exchange; auth refused as expected)" "No auth methods" "$B/SSHERR.TXT"; fi
if [ -f "${RCMD_X:-${TMPDIR:-/tmp}/tistest/rcmd.x}" ]; then
  ok "rcmd.x over TSP: echo" "PASS rcmd: echo" "$log"
  ok "rcmd.x over TSP: x68put 300KB" "PASS rcmd: x68put" "$log"
fi
[ $rc -eq 0 ] || echo "(xeij_pico.sh rc=$rc, log $log)"
echo "$fails failure(s)"
exit $((fails > 0))
