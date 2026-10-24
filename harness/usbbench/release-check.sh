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
# 環境変数: PCAP=1 でケースごとに 445 番の通信を <case>.pcap に (tcpdump)。POLL=0 で空きヒープの問い合わせ (UDP 6869 の status) をしない。UART の keepalive の行 (mallinfo、0047 から
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
cleanup() { smb_unblock; sudo umount -l /mnt/zpico 2>/dev/null; [ -n "${hp:-}" ] && kill $hp 2>/dev/null; [ -n "${uw:-}" ] && kill $uw 2>/dev/null; }
trap cleanup EXIT

usb_up() { lsusb | grep -q cafe:4002; }
revive() {   # Pico は動いているのにハブがポートを諦めた (error -32、unable to enumerate) とき: ポートを入れ直す
  for i in 1 2 3; do usb_up && return 0
    echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 10; done; usb_up; }
# vfat の前に: ブロックデバイスがあるか (zhost が usb-storage を外したままだと USB にはいても /dev/sdX が無い)。無ければポートを入れ直す
wait_blk() { for i in 1 2 3; do for j in $(seq 20); do [ -e $DEV ] && return 0; sleep 1; done
  echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 5; done; [ -e $DEV ]; }
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
begin() { case_name=$1; u0=$(wc -l < $UART); t_case=$(date +%s); resets=()
  # PCAP=1: Pico と smbd の間 (445 番) をケースごとに記録 (30 秒止まるときに、どちらが待っているかを見る)
  [ "${PCAP:-0}" = 1 ] && { sudo tcpdump -i any -s 200 -w "$out/$case_name.pcap" host $PICO_IP and port 445 >/dev/null 2>&1 & tp=$!; }
  usbwatch & uw=$!; }
mark_reset() { resets+=($(date +%s)); }
finish() {   # $1 = そのケースの道具の結果 (OK / 説明)
  local res=$1 why=()
  kill $uw 2>/dev/null; uw=
  [ -n "${tp:-}" ] && { sudo kill $tp 2>/dev/null; tp=; }
  [ -s "$out/usbdrop.$case_name" ] && why+=("USB dropped ($(head -1 "$out/usbdrop.$case_name"))")
  tail -n +$((u0 + 1)) $UART > "$out/$case_name.uart"
  # dmesg は drop_caches の行であふれるので、時刻で切れる journalctl -k から (drop_caches の行は捨てる)
  sudo journalctl -k --since "@$t_case" -o short-precise --no-pager 2>/dev/null | grep -v drop_caches > "$out/$case_name.dmesg"
  grep -aiq "out of memory\|panic\|hardfault\|assert" "$out/$case_name.uart" && why+=("$(grep -aim1 'out of memory\|panic\|hardfault\|assert' "$out/$case_name.uart" | cut -c1-80)")
  # USB の切断: 自分でリセットした直後 (20 秒) 以外
  local disc; disc=$(grep -c "usb 1-1.3: USB disconnect" "$out/$case_name.dmesg")
  local expected=${#resets[@]}
  [ "$disc" -gt "$expected" ] && why+=("USB disconnects $disc (expected $expected)")
  grep -q "unable to enumerate" "$out/$case_name.dmesg" && why+=("hub gave up on the port (re-enabled)")
  usb_up || { why+=("USB gone at the end"); recover; }
  [ "$res" != OK ] && why+=("$res")
  # Linux の SCSI の時間切れ (30 秒) で機器をリセットした回数 (失敗ではないが、30 秒を超えて返事が止まったことを示す)
  local rst; rst=$(grep -c "reset full-speed USB device" "$out/$case_name.dmesg")
  [ "$rst" -gt 0 ] && say "  $case_name: Linux reset the device $rst time(s) ($(grep -m1 'reset full-speed' "$out/$case_name.dmesg" | cut -c8-22))"
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
  # SMB を止めるケースでは、止めている間の長い待ち (NOT READY の繰り返し) は正しい動き: 止まりは数えない
  [ -z "$allow" ] && grep -q "HANG\|STALL" "$log" && r+=("$(grep -c STALL "$log") stalls")
  [ ${#r[@]} -eq 0 ] && echo OK || (IFS='; '; echo "${r[*]}")
}
ZFAT="sudo python3 $here/zfat.py --src $SRC"
ZHOST="sudo $HOME/venv-usbbench/bin/python $here/zhost.py"   # pyusb は venv にだけある

# ---- USB が消えたときの立て直し ----
# Pico が PANIC すると USB から消え、読み書きの途中の python は (デバイスが残ったまま) 何時間も戻らない (10/24 02:07 に 16 時間)。
# マウントを lazy で外し、Pico を SWD でリセットし、ハブのポートを入れ直す。古いデバイスが消えると止まっていた I/O は EIO で戻る
recover() {
  say "  $case_name: recovering (lazy umount, SWD reset, hub port)"
  sudo umount -l /mnt/zpico 2>/dev/null
  reset_pico; sleep 3; revive || { echo 1 | sudo tee $PORT/disable >/dev/null; sleep 3; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 10; }
}
# USB の見張り: ケースの間ずっと、2 秒ごとに lsusb。30 秒続けて見えなければ記録して立て直す (自分でリセットした直後は数えない)
usbwatch() { local gone=0
  while :; do
    if usb_up; then gone=0; else gone=$((gone + 2)); fi
    if [ $gone -ge 30 ]; then echo "$(date +%T) USB gone for 30 s" >> "$out/usbdrop.$case_name"; recover >/dev/null 2>&1; gone=0; fi
    sleep 2; done; }
# runlim <秒> <ログ> <コマンド...>: 裏で動かし、時間を過ぎたら kill -9 して立て直す (D 状態のままでも待たない)。終了コード、時間切れは 124
runlim() { local lim=$1 log=$2; shift 2
  "$@" > "$log" 2>&1 & local p=$! t=0
  while kill -0 $p 2>/dev/null; do sleep 2; t=$((t + 2))
    if [ $t -ge $lim ]; then echo "$(date +%T) runlim: killed after $lim s" >> "$log"
      sudo pkill -9 -P $p 2>/dev/null; kill -9 $p 2>/dev/null; recover; disown $p 2>/dev/null; return 124; fi; done
  wait $p; }

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
    runlim 300 "$out/replay.log" $ZHOST -p $here/profiles/z-fork.json --speed 2 -t 5
    mark_reset; mark_reset    # zhost は最初と最後にバスリセットする
    r=$(grep "^RESULT" "$out/replay.log"); finish "$([[ $r == "RESULT OK"* ]] && echo OK || echo "zhost: ${r:-no result ($(tail -1 "$out/replay.log" | cut -c1-60))}")" ;;
  vfat-boot)
    wait_blk; runlim 400 "$out/vfat-boot.log" $ZFAT --boot --ops 800 --seed 21 & p=$!
    sleep 1; reset_pico; mark_reset
    ( sleep 30; usb_up || revive ) ; wait $p; finish "$(zfat_res "$out/vfat-boot.log" eio-ok-before-open)" ;;
  vfat)
    sleep 5; wait_blk; runlim 600 "$out/vfat.log" $ZFAT --ops 1200 --seed 22; finish "$(zfat_res "$out/vfat.log")" ;;
  smb-late)
    smb_block
    runlim 400 "$out/smb-late.log" $ZFAT --boot --ops 100000 --duration 150 --hang 240 --seed 23 & p=$!
    sleep 1; reset_pico; mark_reset
    sleep 45; smb_unblock; say "  smb-late: SMB unblocked at +45 s"
    wait $p; finish "$(zfat_res "$out/smb-late.log" eio-ok)" ;;
  smb-lost)
    wait_blk; runlim 400 "$out/smb-lost.log" $ZFAT --ops 100000 --duration 180 --hang 240 --seed 24 & p=$!
    sleep 40; smb_block; say "  smb-lost: SMB blocked at +40 s"; sleep 45; smb_unblock; say "  smb-lost: unblocked at +85 s"
    wait $p; finish "$(zfat_res "$out/smb-lost.log" eio-ok)" ;;
  soak)
    wait_blk; runlim 800 "$out/soak.log" $ZFAT --ops 1000000 --duration 600 --rate 2 --seed 25
    finish "$(zfat_res "$out/soak.log")" ;;
  *) say "unknown case $c" ;;
  esac
  revive >/dev/null || true
done
[ -n "${hp:-}" ] && kill $hp 2>/dev/null; hp=
h=$(grep -o 'heap=[0-9]*' "$out/heap.log" 2>/dev/null | cut -d= -f2 | sort -n | head -1)
say "sbrk headroom (status heap=): min ${h:-n/a}; UART mallinfo lines: $(cat "$out"/*.uart 2>/dev/null | grep -aic 'arena\|mallinfo')"
say "result: $fails failing case(s); logs in $out"
exit $fails
