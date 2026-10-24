#!/bin/sh
# boot/serial/raspi/setup.sh -- X68000 (Compact XVI など) を RS-232C でつなぐ Raspberry Pi を用意する。
# Raspberry Pi OS / Debian で、何度実行しても同じ状態になる。
#
#   sudo ./setup.sh [--dev /dev/ttyUSB0] [--speed 19200] [--mode console|slip] [--user NAME]
#                   [--data /data] [--overlay on|off]
#
#   --dev      X68000 につないだシリアルポート。USB シリアルは /dev/serial/by-id/... を使うと挿し直しても名前が変わらない
#   --speed    X68000 の RS-232C の速さ (bootstrap 後は 19200)
#   --mode     起動時にどちらを動かすか。console = ログ係 (x68-serlog)、slip = SLIP + NAT (ppp-x68k)。後から x68mode で変えられる
#   --user     dialout グループに入れる利用者 (既定は sudo した人)
#   --data     書き込みを残す場所 (ログなど)。ラベル X68DATA のパーティションがあれば、ここにマウントするよう fstab に書く
#   --overlay  on: ルートファイルシステムを読み出し専用にし、書き込みは RAM に置く (raspi-config の overlayfs)。
#              電源を急に切っても SD カードが壊れにくい。設定を変えるときは --overlay off で戻して再起動する
#
# 入れるもの:
#   /usr/local/bin/sx        sx.py (rx.x とのやり取り: run / put / get、bootstrap など)。/usr/local/lib/x68k-serial に本体
#   /usr/local/bin/x68type   コンソール (CTTY AUX) に 1 文字ずつ打つ
#   /usr/local/bin/x68con    コンソールのログを追いかけて表示する
#   /usr/local/bin/x68 / x68put   rcmd.x (SLIP の上の TCP) でコマンド実行 / ファイル送信
#   /usr/local/bin/x68mode   console / slip の切り替え
#   x68-serlog.service       ログ係 (/etc/default/x68k-serial)
#   ppp-x68k.service         SLIP (slattach) + IP 転送 + NAT (/etc/default/ppp-x68k)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
serial=$(cd "$here/.." && pwd)
top=$(cd "$serial/../.." && pwd)

DEV= SPEED= MODE= USERNAME=${SUDO_USER:-} DATA= OVERLAY=
while [ $# -gt 0 ]; do
  case "$1" in
    --dev) DEV=$2; shift 2 ;;
    --speed) SPEED=$2; shift 2 ;;
    --mode) MODE=$2; shift 2 ;;
    --user) USERNAME=$2; shift 2 ;;
    --data) DATA=$2; shift 2 ;;
    --overlay) OVERLAY=$2; shift 2 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "setup.sh: unknown option $1" >&2; exit 2 ;;
  esac
done
[ "$(id -u)" = 0 ] || { echo "setup.sh: run as root (sudo)" >&2; exit 1; }
case "${MODE:-console}" in console|slip) ;; *) echo "setup.sh: --mode console|slip" >&2; exit 2 ;; esac
# --mode が無ければ今の状態を保つ (初回は console)
if [ -z "$MODE" ]; then
  MODE=console
  [ -d /run/systemd/system ] && systemctl is-enabled ppp-x68k.service >/dev/null 2>&1 && MODE=slip
fi
case "${OVERLAY:-}" in ""|on|off) ;; *) echo "setup.sh: --overlay on|off" >&2; exit 2 ;; esac
say() { echo "setup.sh: $*"; }
have_systemd() { [ -d /run/systemd/system ]; }

# raspi-config の overlayfs が有効な間は、/etc などへの書き込みが再起動で消える
if grep -q 'overlayroot=tmpfs' /proc/cmdline 2>/dev/null && [ "$OVERLAY" != off ]; then
  echo "setup.sh: the root filesystem is an overlay (read-only mode). Run 'sudo $0 --overlay off', reboot, then run again." >&2
  exit 1
fi

# --- パッケージ (足りないものだけ) ---
pkgs=
for p in python3 net-tools iptables iproute2 procps curl lhasa; do
  dpkg -s "$p" >/dev/null 2>&1 || pkgs="$pkgs $p"
done
if [ -n "$pkgs" ]; then
  say "installing$pkgs"
  apt-get update -q
  DEBIAN_FRONTEND=noninteractive apt-get install -y -q $pkgs
fi

# --- 設定 (/etc/default/x68k-serial)。指定のあった項目だけ書き換え、ほかは前の値を残す ---
conf=/etc/default/x68k-serial
[ -f $conf ] || cat > $conf <<'EOF'
# boot/serial/raspi/setup.sh が作る。x68serlog / sx / x68type / x68con が読む
X68_DEV=/dev/ttyUSB0
X68_BAUD=19200
X68_LOG=/var/log/x68k/x68con.log
X68_HOST=192.168.68.2
SX_LOCK=/run/x68k-serial/lock
EOF
setkey() {   # setkey FILE KEY VALUE
  if grep -q "^$2=" "$1"; then sed -i "s|^$2=.*|$2=$3|" "$1"; else echo "$2=$3" >> "$1"; fi
}
[ -n "$DEV" ] && setkey $conf X68_DEV "$DEV"
[ -n "$SPEED" ] && setkey $conf X68_BAUD "$SPEED"
[ -n "$DATA" ] && setkey $conf X68_LOG "$DATA/log/x68con.log"
. $conf

# SLIP (net/raspi/ppp-x68k.sh) の設定。X68000 の NETUP.BAT (sx stage4) と同じアドレスと速さにする
pconf=/etc/default/ppp-x68k
[ -f $pconf ] || cat > $pconf <<'EOF'
# boot/serial/raspi/setup.sh が作る。ppp-x68k.service (net/raspi/ppp-x68k.sh) が読む
MODE=slip
LOCAL=192.168.68.1
REMOTE=192.168.68.2
MTU=1500
EOF
setkey $pconf DEV "$X68_DEV"
setkey $pconf SPEED "$X68_BAUD"

# --- ファイル ---
lib=/usr/local/lib/x68k-serial
install -d $lib
for f in sx.py rx.bas rx.x getlha.sh getnet.sh README.md; do
  [ -f "$serial/$f" ] && install -m 644 "$serial/$f" "$lib/$f"
done
chmod 755 $lib/sx.py $lib/getlha.sh 2>/dev/null || true
for d in files files-net; do   # Mac で用意したもの (getlha.sh / getnet.sh) があれば一緒に置く
  if [ -d "$serial/$d" ]; then install -d "$lib/$d"; cp -p "$serial/$d"/* "$lib/$d/" 2>/dev/null || true; fi
done
cat > /usr/local/bin/sx <<'EOF'
#!/bin/sh
# sx: boot/serial/sx.py を /etc/default/x68k-serial のポートと速さで動かす (--port / --baud で上書きできる)
[ -r /etc/default/x68k-serial ] && . /etc/default/x68k-serial
export SX_PORT=${SX_PORT:-$X68_DEV} SX_BAUD=${SX_BAUD:-$X68_BAUD} SX_LOCK=${SX_LOCK:-/run/x68k-serial/lock}
exec python3 /usr/local/lib/x68k-serial/sx.py "$@"
EOF
cat > /usr/local/bin/x68put <<'EOF'
#!/bin/sh
# x68put: harness/raspi/x68put.sh。X68000 に tar.x が無いので、既定では圧縮しないで送る (X68PUT_RAW=1)
[ -r /etc/default/x68k-serial ] && . /etc/default/x68k-serial
export X68_HOST=${X68_HOST:-192.168.68.2} X68PUT_RAW=${X68PUT_RAW:-1}
exec /usr/local/lib/x68k-serial/x68put.sh "$@"
EOF
cat > /usr/local/bin/x68 <<'EOF'
#!/bin/sh
# x68: harness/raspi/x68.sh (rcmd.x のクライアント)
[ -r /etc/default/x68k-serial ] && . /etc/default/x68k-serial
export X68_HOST=${X68_HOST:-192.168.68.2}
exec /usr/local/lib/x68k-serial/x68.sh "$@"
EOF
cat > /usr/local/bin/x68con <<'EOF'
#!/bin/sh
# x68con: コンソールのログを追いかけて表示する (harness/raspi/x68con)
[ -r /etc/default/x68k-serial ] && . /etc/default/x68k-serial
exec python3 /usr/local/lib/x68k-serial/x68con.py "${1:-${X68_LOG:-/var/log/x68k/x68con.log}}"
EOF
install -m 755 "$top/harness/raspi/x68.sh" $lib/x68.sh
install -m 755 "$top/harness/raspi/x68put.sh" $lib/x68put.sh
install -m 755 "$top/harness/raspi/x68con" $lib/x68con.py
chmod 755 /usr/local/bin/sx /usr/local/bin/x68put /usr/local/bin/x68 /usr/local/bin/x68con
install -d /etc/systemd/system /etc/tmpfiles.d /usr/local/sbin
install -m 755 "$here/x68type" /usr/local/bin/x68type
install -m 755 "$here/x68mode" /usr/local/bin/x68mode
install -m 755 "$here/x68serlog" /usr/local/sbin/x68serlog
install -m 755 "$top/net/raspi/ppp-x68k.sh" /usr/local/sbin/ppp-x68k.sh
install -m 644 "$here/x68-serlog.service" /etc/systemd/system/x68-serlog.service
# net/raspi/ppp-x68k.service は Pi の UART (ttyS0) 用なので、ポートを選ばない形で書く
cat > /etc/systemd/system/ppp-x68k.service <<'EOF'
# SLIP (net/raspi/ppp-x68k.sh: slattach + IP 転送 + NAT)。設定は /etc/default/ppp-x68k。boot/serial/raspi/setup.sh が作る
[Unit]
Description=SLIP for X68000 on the serial port
After=network.target
Conflicts=x68-serlog.service

[Service]
EnvironmentFile=-/etc/default/ppp-x68k
Environment=NO_GETTY_RESTORE=1
ExecStart=/usr/local/sbin/ppp-x68k.sh
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
echo 'd /run/x68k-serial 1777 root root -' > /etc/tmpfiles.d/x68k-serial.conf
install -d -m 1777 /run/x68k-serial
install -d "$(dirname "$X68_LOG")"

# --- 利用者をシリアルポートのグループに ---
if [ -n "$USERNAME" ] && id "$USERNAME" >/dev/null 2>&1; then
  id -nG "$USERNAME" | grep -qw dialout || { usermod -aG dialout "$USERNAME"; say "added $USERNAME to dialout (log in again)"; }
fi

# --- Pi 自身の UART (GPIO) を使うときは、シリアルコンソール (ログインと起動メッセージ) を止める ---
case "$X68_DEV" in
  */ttyS0|*/ttyAMA0|*/serial0)
    if command -v raspi-config >/dev/null; then
      raspi-config nonint do_serial_hw 0     # UART を有効に
      raspi-config nonint do_serial_cons 1   # その上のコンソールは無効に
      say "Pi UART enabled, serial console disabled (reboot needed)"
    else
      say "disable the serial console on $X68_DEV yourself (console=serial0 in cmdline.txt, serial-getty)"
    fi ;;
esac

# --- 書き込みを残す場所 ---
if [ -n "$DATA" ]; then
  install -d "$DATA"
  if command -v blkid >/dev/null && blkid -L X68DATA >/dev/null 2>&1; then
    grep -q "LABEL=X68DATA" /etc/fstab || echo "LABEL=X68DATA $DATA ext4 defaults,noatime,nofail 0 2" >> /etc/fstab
    mountpoint -q "$DATA" || mount "$DATA" || true
    say "LABEL=X68DATA is mounted on $DATA"
  else
    say "no partition labelled X68DATA; $DATA is a plain directory (it will be in RAM when --overlay on)"
  fi
  install -d "$DATA/log" "$DATA/piscsi"
fi

# --- サービス (ログインしなくても起動時に動く) ---
if have_systemd; then
  systemctl daemon-reload
  systemd-tmpfiles --create /etc/tmpfiles.d/x68k-serial.conf
  /usr/local/bin/x68mode "$MODE"
  /usr/local/bin/x68mode status
else
  say "systemd is not running; services are installed but not started"
fi

# --- 読み出し専用のルート (最後に行う。有効にした後の /etc の変更は再起動で消えるため) ---
if [ "$OVERLAY" = on ] || [ "$OVERLAY" = off ]; then
  if command -v raspi-config >/dev/null; then
    [ "$OVERLAY" = on ] && raspi-config nonint do_overlayfs 0 || raspi-config nonint do_overlayfs 1
    say "overlay root $OVERLAY: takes effect after reboot"
  else
    say "raspi-config not found; the read-only root is not changed (use Raspberry Pi OS for --overlay)"
  fi
fi
say "done ($X68_DEV ${X68_BAUD}bps, mode $MODE)"
