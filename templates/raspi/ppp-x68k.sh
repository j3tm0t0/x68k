#!/bin/bash
# X68000 Z からの SLIP (slip.sys + slipd.x) / PPP (ppp.sys + ppp.x) 接続を Raspberry Pi の GPIO UART で受け、
# NAT で外 (インターネット) へ抜けられるようにする。Ctrl-C で終了し serial-getty を戻す。
#   sudo ./ppp-x68k.sh                # /dev/ttyS0, SLIP 38400bps (X68000 Z の既定の netup と対)
#   sudo SPEED=19200 ./ppp-x68k.sh    # X68000 側を NETSPEED=19200 で作ったとき
#   sudo MODE=ppp SPEED=9600 ./ppp-x68k.sh   # X68000 側が ppp.sys (NET=ppp) のとき
#   DEV=/dev/ttyUSB0 sudo ./ppp-x68k.sh
#   SLIP は slattach (net-tools) とカーネルの slip モジュールを使う。systemd で常駐させるなら ppp-x68k.service
#   (設定は /etc/default/ppp-x68k に MODE= / SPEED= / MTU=)
# 前提:
#   - /boot/firmware/config.txt に enable_uart=1。cmdline.txt から console=serial0,... は削除する
#     (カーネルメッセージが PPP フレームを壊す)
#   - ppp パッケージ (pppd) が入っていること: sudo apt install ppp
#   - X68000 側は /etc/conf.ppp の raspi: ラベル (192.168.68.2) と対になる
set -eu
DEV=${DEV:-/dev/ttyS0}
MODE=${MODE:-slip}        # slip | ppp
[ "$MODE" = slip ] && IFN=sl0 || IFN=ppp0
SPEED=${SPEED:-38400}     # X68000 側の netup <速度> (make image の NETSPEED=) と同じにする
MTU=${MTU:-1500}          # 296 に絞ると ESP/X の TCP が rcmd 接続 1 回で止まった (MSS 256 を扱えない疑い)
LOCAL=${LOCAL:-192.168.68.1}     # Raspberry Pi 側 (PPP)
REMOTE=${REMOTE:-192.168.68.2}   # X68000 側
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
GETTY="serial-getty@$(basename "$DEV").service"
restore() {
  [ -n "${TS_PID:-}" ] && kill "$TS_PID" 2>/dev/null
  [ -n "${SL_PID:-}" ] && kill "$SL_PID" 2>/dev/null
  # systemd のサービス (ppp-x68k.service) から動かすときは getty を戻さない (Restart で再起動するため)
  if [ -z "${NO_GETTY_RESTORE:-}" ]; then
    echo "restoring $GETTY"
    systemctl start "$GETTY" 2>/dev/null || true
  fi
}
trap restore EXIT INT TERM
systemctl stop "$GETTY" 2>/dev/null || true
# X68000 → Pi → 外: フォワード + NAT (再起動で消える。恒久化するなら sysctl.conf / nftables に)
sysctl -q -w net.ipv4.ip_forward=1
iptables -t nat -C POSTROUTING -s "$REMOTE" -j MASQUERADE 2>/dev/null ||
  iptables -t nat -A POSTROUTING -s "$REMOTE" -j MASQUERADE
# X68000 宛ての ping (ICMP echo request) を捨てる。ESP/X は rcmd で TCP 接続を 1 回閉じたあとに
# echo request を受けると TCP/IP ドライバごと止まった (X68000 Z / 2026-10-19)。X68000 からの ping は通る
for ch in OUTPUT FORWARD; do
  iptables -C $ch -d "$REMOTE" -p icmp --icmp-type echo-request -j DROP 2>/dev/null ||
    iptables -A $ch -d "$REMOTE" -p icmp --icmp-type echo-request -j DROP
done
# 時刻合わせ: リンクが上がって X68000 の rcmd.x (port 2323) が応答したら、Pi の時刻を @time で送る (リンクごとに 1 回)
timesync() {
  synced=""
  while sleep 5; do
    id=$(cat /sys/class/net/$IFN/ifindex 2>/dev/null) || { synced=""; continue; }
    if [ "$id" = "$synced" ]; then continue; fi   # set -e の下で && continue を使うとループごと終了する
    if exec 3<>"/dev/tcp/$REMOTE/2323" 2>/dev/null; then
      printf '@time %s\r\n' "$(date '+%Y %m %d %H %M %S' | sed 's/ 0/ /g')" >&3
      timeout 10 cat <&3 >/dev/null; exec 3>&-
      echo "timesync: X68000 clock set to $(date '+%F %T')"; synced=$id
    fi
  done
}
timesync &
TS_PID=$!
if [ "$MODE" = slip ]; then
  # SLIP: slattach が tty を sl0 にし、あとは普通のネットワークインターフェイスとして設定する。
  # 交渉も生存確認も無いので、X68000 が再起動しても待ち受けのまま使える
  echo "SLIP on $DEV ($SPEED bps)  $LOCAL <-> $REMOTE  (Ctrl-C to stop)"
  modprobe slip 2>/dev/null || true
  /usr/sbin/slattach -s "$SPEED" -p slip -L "$DEV" &
  SL_PID=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do ip link show sl0 >/dev/null 2>&1 && break; sleep 1; done
  # IPv6 を止める (X68000 の ESP/X は IPv4 しか扱えない。sl0 が上がると IPv6 のルータ要請などが流れる)
  sysctl -q -w net.ipv6.conf.sl0.disable_ipv6=1 2>/dev/null || true
  ip addr flush dev sl0 2>/dev/null || true
  ip addr add "$LOCAL" peer "$REMOTE" dev sl0
  ip link set sl0 mtu "$MTU" up
  wait "$SL_PID"
  exit 0
fi
echo "waiting for PPP on $DEV ($SPEED bps)  $LOCAL <-> $REMOTE  (Ctrl-C to stop)"
# local: モデム制御線を見ない / nocrtscts: ハードフロー制御なし (3 線) / novj noccp: ppp.x が拒否する機能を最初から出さない
# persist maxfail 0: X68000 側が来なくても切断しても待ち続ける (maxfail 既定 10 だと 10 回失敗で pppd が終了する)。 (exec にすると EXIT trap が動かず getty が戻らない)
pppd "$DEV" "$SPEED" mtu "$MTU" local noauth nodetach nocrtscts novj noccp noipv6 nodefaultroute persist maxfail 0 \
  lcp-echo-interval 15 lcp-echo-failure 4 "$LOCAL:$REMOTE"
