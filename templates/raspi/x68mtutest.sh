#!/bin/bash
# x68mtutest.sh [MTU ...] : PPP でつながった X68000 (rcmd.x 起動中) に対し、ppp0 の MTU を変えながら
#   (1) MTU いっぱいの分割されない ping  (2) rcmd 経由の 20KB 転送 (x68put) を試す。
#   落ちた / 固まったところで止め、通った最大の MTU に戻す。sudo (ip link) が必要。
HOST=${X68_HOST:-192.168.68.2}
MTUS=${*:-296 400 576 800 1006 1200 1500}
SIZE=${TESTSIZE:-20480}
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
head -c "$SIZE" /dev/urandom > "$tmp"
best=""
alive() { ping -c 2 -W 3 "$HOST" >/dev/null 2>&1; }
printf '%6s  %-12s  %s\n' MTU ping transfer
for m in $MTUS; do
  sudo ip link set ppp0 mtu "$m" || exit 1
  ok=$(ping -M do -c 5 -i 1 -s $((m - 28)) -W 4 "$HOST" 2>/dev/null | grep -o '[0-9]* received' | cut -d' ' -f1)
  t0=$(date +%s.%N)
  if timeout 120 ~/bin/x68put "$tmp" /tmp/mtutest.bin >/dev/null 2>&1 &&
     got=$(timeout 30 ~/bin/x68 'ls -l /tmp/mtutest.bin' 2>/dev/null | awk '{print $2}') && [ "$got" = "$SIZE" ]; then
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    xfer=$(printf '%.1fs %.0f B/s = %.1f kbps' "$t" "$(echo "$SIZE / $t" | bc -l)" "$(echo "$SIZE * 8 / $t / 1000" | bc -l)")
  else
    xfer="FAIL"
  fi
  printf '%6s  %-12s  %s\n' "$m" "${ok:-0}/5" "$xfer"
  if [ "${ok:-0}" -lt 4 ] || [ "$xfer" = FAIL ]; then
    alive && echo "  -> X68000 は生きている (この MTU は不可)" || echo "  -> X68000 が応答しない。固まった可能性 (電源を入れ直す)"
    break
  fi
  best=$m
done
if [ -n "$best" ]; then
  sudo ip link set ppp0 mtu "$best"; echo "MTU を $best に戻した (ppp-x68k.sh で使うなら: sudo MTU=$best ~/ppp-x68k.sh)"
fi
