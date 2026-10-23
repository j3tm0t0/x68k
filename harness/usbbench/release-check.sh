#!/bin/bash
# release-check.sh <elf> [ケース...]: fork (x68kzremotedrv) のファームを出す前の確認。raspi3 から机の上の p2w に (Z は使わない)
#
# p2w に <elf> を書き、ケースごとに Pico を起動し直して、ホストの使い方と SMB の状態を変えて叩く。
# 合格 = Out of memory / PANIC / HardFault が UART に出ない、USB が勝手に消えない、読んだ中身が元の HDS と同じ (CRC)。
#
# ケース (既定は全部、順に):
#   idle        起動して 60 秒なにもしない (ハブの列挙と Linux の TEST UNIT READY だけ)
#   replay      Z で採った USB の使い方 (profiles/z-fork.json) を zhost.py で送り直す
#   vfat-boot   リセット直後 (HDS が開く前、大きさが仮の 0xfffffe00) に vfat でマウントして zfat.py で読み書き
#   vfat        起動が落ち着いてから vfat で読み書き
#   smb-late    SMB を止めた (raspi3 の smbd への 445 を DROP) まま起動し、45 秒後に戻す。その間も vfat で読み書き
#   smb-lost    vfat の読み書きの途中で SMB を 45 秒止めて戻す
#   soak        10 分: vfat をゆっくり (2 回/秒) 読み書きしながら、空きヒープを 10 秒ごとに見る
# SD カードの有無: p2w には SD が無い (SD ありのケースは sd 基板でしか試せない。結果に「SD 無し」と書く)
#
# 前提 (raspi3): ~/bin の swdflash / x68key、~/usbbench の zhost.py / zfat.py / profiles、sudo、p2w の Probe、
#   SMB の共有の実ファイル /srv/x68000z/pzltest.hds (p2w の設定の HDS0)。p2w は scratchpad/p2wlock を取ってから
# 環境変数: POLL=0 で空きヒープの問い合わせ (UDP 6869 の status) をしない。UART の keepalive の行 (mallinfo、0047 から
#   タスクのスタックの残り) も uart に残る
# 結果: ~/release-check/<elf の名前>-<時刻>/ (ケースごとのログ、uart.log、heap.log、summary.txt)。終了コード = 不合格の数
set -u
export PATH=$HOME/bin:$PATH
elf=$1; shift
cases=${*:-idle replay vfat-boot vfat smb-late smb-lost soak}
here=$HOME/usbbench
SRC=/srv/x68000z/pzltest.hds
PICO_IP=${PICO_IP:-192.168.1.3}
UART=/tmp/picouart-p2w.log
PORT=/sys/bus/usb/devices/1-1:1.0/1-1-port3
DEV=/dev/disk/by-id/usb-X68000Z_Remote_Drive_Mas_123456789012-0:0
out=$HOME/release-check/$(basename "$elf" .elf)-poll${POLL:-1}-$(date +%m%d-%H%M); mkdir -p "$out"
fails=0
say() { echo "$(date +%T) $*" | tee -a "$out/summary.txt"; }

smb_block()   { sudo iptables -C INPUT -s $PICO_IP -p tcp --dport 445 -j DROP 2>/dev/null || sudo iptables -I INPUT -s $PICO_IP -p tcp --dport 445 -j DROP; }
smb_unblock() { while sudo iptables -C INPUT -s $PICO_IP -p tcp --dport 445 -j DROP 2>/dev/null; do sudo iptables -D INPUT -s $PICO_IP -p tcp --dport 445 -j DROP; done; }
cleanup() { smb_unblock; sudo umount /mnt/zpico 2>/dev/null; [ -n "${hp:-}" ] && kill $hp 2>/dev/null; }
trap cleanup EXIT

usb_up() { lsusb | grep -q cafe:4002; }
revive() {   # Pico は動いているのにハブがポートを諦めた (error -32、unable to enumerate) とき: ポートを入れ直す
  for i in 1 2 3; do usb_up && return 0
    echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 10; done; usb_up; }
wait_usb() { for i in $(seq ${1:-40}); do usb_up && return 0; sleep 1; done; revive; }
openocd_p2w() { local s; s=$(awk '$1=="p2w"{print $2}' ~/.picolog)
  (cd ~/src/openocd-rp && LD_LIBRARY_PATH=. timeout 60 ./openocd -s scripts -f interface/cmsis-dap.cfg -c "adapter serial $s" \
     -f target/rp2350.cfg -c "gdb_port disabled" -c "adapter speed 5000" -c "$1" 2>&1); }
reset_pico() { openocd_p2w "init; reset; exit" > /dev/null; }   # 走っている Pico を止めずにリセット (SYSRESETREQ)
flash() { for i in 1 2 3; do swdflash "$elf" p2w > "$out/flash.log" 2>&1
  grep -q "Verified OK" "$out/flash.log" && ! grep -q "Error" "$out/flash.log" && return 0; done; return 1; }

# ---- 見張り: 空きヒープ (UDP 6869 の status の heap=) を 10 秒ごと ----
heaplog() { while :; do echo "$(date +%T) $(x68key -t bench status 2>/dev/null | grep -o 'heap=[0-9]*\|uptime=[0-9]*s' | tr '\n' ' ')" >> "$out/heap.log"; sleep 10; done; }
# POLL=0: UDP 6869 に何も送らない (status の問い合わせ自体が不具合を起こしうる: fork 0047 の status タスクのスタック)
POLL=${POLL:-1}
[ "$POLL" = 1 ] && { heaplog & hp=$!; }

# ケースの前後: UART と dmesg の位置を覚え、あとで PANIC と USB の切断を探す
begin() { case_name=$1; u0=$(wc -l < $UART); k0=$(sudo dmesg | wc -l); t_case=$(date +%s); resets=(); sudo dmesg -T >/dev/null; }
mark_reset() { resets+=($(date +%s)); }
finish() {   # $1 = そのケースの道具の結果 (OK / 説明)
  local res=$1 why=()
  tail -n +$((u0 + 1)) $UART > "$out/$case_name.uart"
  sudo dmesg | tail -n +$((k0 + 1)) > "$out/$case_name.dmesg"
  grep -aiq "out of memory\|panic\|hardfault\|assert" "$out/$case_name.uart" && why+=("$(grep -aim1 'out of memory\|panic\|hardfault\|assert' "$out/$case_name.uart" | cut -c1-80)")
  # USB の切断: 自分でリセットした直後 (20 秒) 以外
  local disc; disc=$(grep -c "usb 1-1.3: USB disconnect" "$out/$case_name.dmesg")
  local expected=${#resets[@]}
  [ "$disc" -gt "$expected" ] && why+=("USB disconnects $disc (expected $expected)")
  grep -q "unable to enumerate" "$out/$case_name.dmesg" && why+=("hub gave up on the port (re-enabled)")
  usb_up || why+=("USB gone at the end")
  [ "$res" != OK ] && why+=("$res")
  if [ ${#why[@]} -eq 0 ]; then say "PASS $case_name"; else say "FAIL $case_name: $(IFS='; '; echo "${why[*]}")"; fails=$((fails + 1)); fi
}
zfat_res() {   # zfat.py のログ → OK / 説明 ($2 = EIO を許す)
  local log=$1 allow=${2:-}
  grep -q "^STATS" "$log" || { echo "zfat did not finish ($(grep -m1 'HANG\|Traceback\|mount failed' "$log" | cut -c1-60))"; return; }
  local m e d; read m e d < <(sed -n 's/^STATS mismatch=\([0-9]*\) eio=\([0-9]*\) smbdiff=\([0-9]*\)/\1 \2 \3/p' "$log")
  local r=()
  [ "$m" != 0 ] && r+=("$m sectors read back wrong ($(grep -m1 '^ *[0-9.]* BAD' "$log" | cut -c16-110))")
  [ "$d" != 0 ] && r+=("$d written sectors differ on the SMB file")
  [ "$e" != 0 ] && [ -z "$allow" ] && r+=("$e I/O errors")
  grep -q "HANG\|STALL" "$log" && r+=("$(grep -c STALL "$log") stalls")
  [ ${#r[@]} -eq 0 ] && echo OK || (IFS='; '; echo "${r[*]}")
}
ZFAT="sudo python3 $here/zfat.py --src $SRC"

say "release-check $(basename "$elf") POLL=$POLL ($(sha256sum "$elf" | cut -c1-8)) on p2w (no SD card); cases: $cases"
smb_unblock
flash || { say "FAIL flash"; exit 99; }
wait_usb 60 || { say "FAIL USB never came up after flashing"; exit 99; }
sleep 20

for c in $cases; do
  begin $c
  case $c in
  idle)
    reset_pico; mark_reset; wait_usb; sleep 60; finish OK ;;
  replay)
    sudo python3 $here/zhost.py -p $here/profiles/z-fork.json --speed 2 -t 5 > "$out/replay.log" 2>&1
    mark_reset; mark_reset    # zhost は最初と最後にバスリセットする
    r=$(grep "^RESULT" "$out/replay.log"); finish "$([ "${r%% *}" = "RESULT" ] && [[ $r == "RESULT OK"* ]] && echo OK || echo "zhost: ${r:-no result}")" ;;
  vfat-boot)
    $ZFAT --boot --ops 800 --seed 21 > "$out/vfat-boot.log" 2>&1 & p=$!
    sleep 1; reset_pico; mark_reset
    ( sleep 30; usb_up || revive ) ; wait $p; finish "$(zfat_res "$out/vfat-boot.log" eio-ok-before-open)" ;;
  vfat)
    sleep 5; $ZFAT --ops 1200 --seed 22 > "$out/vfat.log" 2>&1; finish "$(zfat_res "$out/vfat.log")" ;;
  smb-late)
    smb_block
    $ZFAT --boot --ops 100000 --duration 150 --hang 240 --seed 23 > "$out/smb-late.log" 2>&1 & p=$!
    sleep 1; reset_pico; mark_reset
    sleep 45; smb_unblock; say "  smb-late: SMB unblocked at +45 s"
    wait $p; finish "$(zfat_res "$out/smb-late.log" eio-ok)" ;;
  smb-lost)
    $ZFAT --ops 100000 --duration 180 --hang 240 --seed 24 > "$out/smb-lost.log" 2>&1 & p=$!
    sleep 40; smb_block; say "  smb-lost: SMB blocked at +40 s"; sleep 45; smb_unblock; say "  smb-lost: unblocked at +85 s"
    wait $p; finish "$(zfat_res "$out/smb-lost.log" eio-ok)" ;;
  soak)
    $ZFAT --ops 1000000 --duration 600 --rate 2 --seed 25 > "$out/soak.log" 2>&1
    finish "$(zfat_res "$out/soak.log")" ;;
  *) say "unknown case $c" ;;
  esac
  revive >/dev/null || true
done
kill $hp 2>/dev/null; hp=
h=$(grep -o 'heap=[0-9]*' "$out/heap.log" 2>/dev/null | cut -d= -f2 | sort -n | head -1)
say "sbrk headroom (status heap=): min ${h:-n/a}; UART mallinfo lines: $(cat "$out"/*.uart 2>/dev/null | grep -aic 'arena\|mallinfo')"
say "result: $fails failing case(s); logs in $out"
exit $fails
