#!/bin/bash
# zfatrun.sh [名前...]: ファームごとに (a) 起動の窓 (リセット直後にマウント) 800 回、(b) 落ち着いてから 1500 回 (raspi3、p2w)
# 名前: fork / pzlnew / pzlold (下の FW)。結果は ~/zfat-results/<名前>-{boot,steady}.log
export PATH=$HOME/bin:$PATH
cd ~/usbbench; out=~/zfat-results; mkdir -p $out
declare -A FW=([fork]=$HOME/fw-fork-4fb2a2f5.elf [pzlnew]=/tmp/pzl-55583f4-new-bench.elf [pzlold]=/tmp/pzl-55583f4-old-bench.elf)
PORT=/sys/bus/usb/devices/1-1:1.0/1-1-port3
cycleport() { echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; }
revive() {   # Pico は動いているのに USB に出てこない (ハブがポートを諦めた、error -32) とき: ポートを入れ直す
  for i in 1 2 3; do lsusb | grep -q cafe:4002 && return 0; cycleport; sleep 10; done; return 1; }
flash() {    # 書き込みが途中で失敗すると Pico が動かなくなる: Verified OK まで 3 回
  for i in 1 2 3; do swdflash $1 p2w > $2 2>&1; grep -q "Verified OK" $2 && ! grep -q "Error" $2 && return 0; done; return 1; }
for n in ${@:-pzlnew pzlold fork}; do
  sudo umount /mnt/zpico 2>/dev/null
  sudo python3 zfat.py --src /srv/x68000z/pzltest.hds --boot --ops 800 --seed 11 > $out/$n-boot.log 2>&1 &
  p=$!; sleep 1
  flash ${FW[$n]} $out/$n-flash.log || { echo "$n: flash failed"; sudo kill $p; continue; }
  for i in $(seq 30); do lsusb | grep -q cafe:4002 && break; sleep 1; done
  lsusb | grep -q cafe:4002 || { echo "$n: re-enabling port 3"; revive; }
  ( sleep 400; sudo kill $p 2>/dev/null ) & k=$!
  wait $p; kill $k 2>/dev/null; echo "$n boot: $(tail -1 $out/$n-boot.log)"
  sudo umount /mnt/zpico 2>/dev/null; sleep 5
  revive || echo "$n: USB did not come back"
  sleep 10
  sudo python3 zfat.py --src /srv/x68000z/pzltest.hds --ops 1500 --seed 12 > $out/$n-steady.log 2>&1
  echo "$n steady: $(tail -1 $out/$n-steady.log)"
done
