#!/bin/sh
# mkboot.sh -- 実機 X68000 用の起動フロッピー (2HD、XDF 1232KB) を作る: Human68k + RS-232C (rx.x / CTTY AUX) + SLIP + Z-OS + ssh.x
#
#   ZOS_X=.../kernel/zos.x ZOSH_X=.../shell/src/zosh.x ./mkboot.sh [出力先ディレクトリ]
#
# 作るもの (出力先、既定は boot/serial/floppy/out):
#   x68k-boot-2m.xdf    主記憶 2MB 用。RAM ディスクなし。Z-OS のペインは 1 つ 640KB (ZOS.BAT)
#   x68k-boot-12m.xdf   主記憶 8MB 以上用。zram.sys で 3MB の RAM ディスク (C:) を作り、よく使う道具を写して PATH の先頭にする
#
# 入力 (環境変数で渡す):
#   ZOS_X / ZOSH_X   Z-OS の核とシェル (j3tm0t0/x68k-zos は非公開なので、組んだものの場所を渡す)。必須
#   SSH_X            ssh.x。無ければ j3tm0t0/x68k のリリース v2026.10.23.1 から取る (SHA256SUMS で確かめる)
#   ZRAM_SYS         zram.sys (x68k-zos の tools/gccspeed)。無ければ 12m 版を作らない
#   HUMAN_XDF        Human68k 3.02 のシステムディスク (起動セクタと HUMAN.SYS / COMMAND.X / RSDRV.SYS を取る)
#   NET              ../files-net (getnet.sh が作る xip.x / ifconfig.x / inetdconf.x / slip.sys / slipd.x / rcmd.x)
#   ZRAM_KB          RAM ディスクの大きさ (KB、既定 3072)
#   X68_IP / X68_PEER / X68_SPEED   SLIP の設定 (既定 192.168.68.2 / 192.168.68.1 / 19200)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
serial=$(cd "$here/.." && pwd)
top=$(cd "$serial/../.." && pwd)
out=$(mkdir -p "${1:-$here/out}" && cd "${1:-$here/out}" && pwd)
: "${ZOS_X:?ZOS_X=zos.x のパス}" "${ZOSH_X:?ZOSH_X=zosh.x のパス}"
HUMAN_XDF=${HUMAN_XDF:-$top/x68kcli/tools/xeij/HUMAN302.XDF}
[ -f "$HUMAN_XDF" ] || HUMAN_XDF=/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF
NET=${NET:-$serial/files-net}
ZRAM_KB=${ZRAM_KB:-3072}
X68_IP=${X68_IP:-192.168.68.2} X68_PEER=${X68_PEER:-192.168.68.1} X68_SPEED=${X68_SPEED:-19200}
for f in XIP.X IFCONFIG.X INETDCONF.X SLIP.SYS SLIPD.X RCMD.X protocols services; do
  [ -f "$NET/$f" ] || { echo "mkboot: $NET/$f が無い (../getnet.sh で作る)" >&2; exit 1; }
done
[ -f "$serial/rx.x" ] || make -s -C "$serial" rx.x
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -z "${SSH_X:-}" ]; then
  rel=https://github.com/j3tm0t0/x68k/releases/download/v2026.10.23.1
  curl -fsSL -o "$work/ssh.x" $rel/ssh.x
  curl -fsSL -o "$work/SHA256SUMS" $rel/SHA256SUMS
  want=$(grep ' ssh.x$' "$work/SHA256SUMS" | cut -d' ' -f1)
  got=$( (sha256sum "$work/ssh.x" 2>/dev/null || shasum -a 256 "$work/ssh.x") | cut -d' ' -f1)
  [ "$want" = "$got" ] || { echo "mkboot: ssh.x sha256 mismatch" >&2; exit 1; }
  SSH_X=$work/ssh.x
fi
crlf() { python3 -c 'import sys; d=sys.stdin.read().replace("\r\n","\n").replace("\n","\r\n"); sys.stdout.buffer.write(d.encode("cp932"))'; }

build() {   # build 名前 zram(0|1)
  t=$work/$1
  mkdir -p "$t/SYS" "$t/BIN" "$t/ETC" "$t/HOME" "$t/TMP"
  python3 "$here/xdfget.py" "$HUMAN_XDF" /HUMAN.SYS "$t/HUMAN.SYS"
  python3 "$here/xdfget.py" "$HUMAN_XDF" /COMMAND.X "$t/COMMAND.X"
  python3 "$here/xdfget.py" "$HUMAN_XDF" /SYS/RSDRV.SYS "$t/SYS/RSDRV.SYS"
  cp "$NET/SLIP.SYS" "$t/SYS/SLIP.SYS"
  for f in XIP.X IFCONFIG.X INETDCONF.X SLIPD.X RCMD.X; do cp "$NET/$f" "$t/BIN/$f"; done
  cp "$NET/protocols" "$NET/services" "$t/ETC/"
  cp "$serial/rx.x" "$t/BIN/RX.X"
  cp "$ZOS_X" "$t/BIN/ZOS.X"
  cp "$ZOSH_X" "$t/BIN/ZOSH.X"
  cp "$SSH_X" "$t/BIN/SSH.X"
  [ "$2" = 1 ] && cp "$ZRAM_SYS" "$t/SYS/ZRAM.SYS"
  # --- CONFIG.SYS ---
  {
    echo "FILES     = 40"
    echo "BUFFERS   = 20 1024"
    echo "LASTDRIVE = Z:"
    echo "ENVSET    = 2048 \\STARTUP.ENV"
    echo "PROCESS   = 16 10 20"
    echo "DEVICE    = \\SYS\\RSDRV.SYS"
    echo "DEVICE    = \\SYS\\SLIP.SYS"
    [ "$2" = 1 ] && echo "DEVICE    = \\SYS\\ZRAM.SYS $ZRAM_KB"
    true
  } | crlf > "$t/CONFIG.SYS"
  printf 'path=\\;\\BIN\r\n' > "$t/STARTUP.ENV"
  # --- AUTOEXEC.BAT ---
  {
    echo "ECHO OFF"
    echo "PATH A:\\;A:\\BIN"
    echo "SET HOME=A:/home"
    echo "SET SYSROOT=A:\\"
    echo "SET temp=A:\\TMP"
    echo "SET TEMP=A:\\TMP"
    echo "SET ZOS_SHELL=A:\\BIN\\ZOSH.X"
    if [ "$2" = 1 ]; then
      echo "REM --- RAM ディスク (zram.sys、$ZRAM_DRIVE:) によく使う道具を写して PATH の先頭にする ---"
      for f in ZOSH.X SSH.X SLIPD.X RX.X; do echo "COPY A:\\BIN\\$f $ZRAM_DRIVE:\\ >NUL"; done
      echo "PATH $ZRAM_DRIVE:\\;A:\\;A:\\BIN"
      echo "SET ZOS_SHELL=$ZRAM_DRIVE:\\ZOSH.X"
    fi
    echo "REM --- boot/serial: RS-232C ${X68_SPEED}bps をコンソールにする (2 秒以内にキーで取りやめ) ---"
    echo "RX -B$X68_SPEED -K2"
    echo "IF ERRORLEVEL 1 GOTO RXKEEP"
    echo "CTTY AUX"
    echo ":RXKEEP"
  } | crlf > "$t/AUTOEXEC.BAT"
  # --- ZOS.BAT (Z-OS を起動する。2MB ではペインを小さく) ---
  {
    echo "REM Z-OS: zosh のペインを 1 つ開いて始める。終えるのは OPT.1 + Q"
    if [ "$2" = 1 ]; then echo "A:\\BIN\\ZOS.X %1 %2 %3 %4"; else echo "A:\\BIN\\ZOS.X -m 640 %1 %2 %3 %4"; fi
  } | crlf > "$t/ZOS.BAT"
  # --- SLIP (段階 4 と同じ)。NETUP で xip.x と SLIP を上げ、rcmd.x を前面で待たせる ---
  {
    echo "REM SLIP ($X68_IP <-> $X68_PEER, ${X68_SPEED}bps) と rcmd.x"
    echo "RX -B$X68_SPEED"
    echo "XIP -w 4096 -n 2"
    echo "IFCONFIG lp0 up"
    echo "IFCONFIG ep0 up $X68_IP"
    echo "IFCONFIG ep0 netmask 128.0.0.0"
    echo "INETDCONF +router $X68_PEER"
    echo "IFCONFIG ep0 netmask 255.255.255.255"
    echo "INETDCONF +dns 8.8.8.8"
    echo "SLIPD"
    echo "RCMD -v -s command.x"
  } | crlf > "$t/NETUP.BAT"
  printf '127.0.0.1\tlocalhost\tlocalhost\r\n%s\tx68k\t\tx68k\r\n%s\traspi\t\traspi\r\n' "$X68_IP" "$X68_PEER" > "$t/ETC/hosts"
  printf '127\t\tloopback\r\n%s\tslip-net\r\n' "$(echo "$X68_IP" | cut -d. -f1-3)" > "$t/ETC/networks"
  crlf < "$here/zoshrc" > "$t/ETC/ZOSHRC"
  crlf < "$here/zosh.rc" > "$t/HOME/ZOSH.RC"
  free=$(python3 "$here/mkxdf.py" "$out/$1.xdf" "$HUMAN_XDF" "$t")
  echo "$1.xdf: ${free}KB free"
}
ZRAM_DRIVE=${ZRAM_DRIVE:-C}
build x68k-boot-2m 0
if [ -n "${ZRAM_SYS:-}" ] && [ -f "$ZRAM_SYS" ]; then build x68k-boot-12m 1; else echo "ZRAM_SYS が無いので x68k-boot-12m.xdf は作らない"; fi
