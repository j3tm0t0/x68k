#!/bin/bash
# zfatrun.sh [names...]: ファームごとに (a) 起動の窓 (リセット直後にマウント) 800 回、(b) 落ち着いてから 1500 回
export PATH=$HOME/bin:$PATH
cd ~/usbbench; out=~/zfat-results; mkdir -p $out
declare -A FW=([fork]=$HOME/fw-fork-4fb2a2f5.elf [pzlnew]=/tmp/pzl-55583f4-new-bench.elf [pzlold]=/tmp/pzl-55583f4-old-bench.elf)
PORT=/sys/bus/usb/devices/1-1:1.0/1-1-port3
revive() {   # Pico が USB に出てこない (ハブがポートを諦めた) とき: ポートの電源を入れ直す
  for i in 1 2 3; do lsusb | grep -q cafe:4002 && return 0
    echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 10; done; return 1; }
for n in "${@:-pzlnew pzlold fork}"; do
  for n1 in $n; do
  sudo umount /mnt/zpico 2>/dev/null
  sudo python3 zfat.py --src /srv/x68000z/pzltest.hds --boot --ops 800 --seed 11 > $out/$n1-boot.log 2>&1 &
  p=$!; sleep 1; swdflash ${FW[$n1]} p2w > $out/$n1-flash.log 2>&1
  ( sleep 120; kill $p 2>/dev/null ) & k=$!
  wait $p; kill $k 2>/dev/null; echo "$n1 boot: $(tail -1 $out/$n1-boot.log)"
  sudo umount /mnt/zpico 2>/dev/null; sleep 5
  revive || echo "$n1: USB did not come back"
  sleep 10
  sudo python3 zfat.py --src /srv/x68000z/pzltest.hds --ops 1500 --seed 12 > $out/$n1-steady.log 2>&1
  echo "$n1 steady: $(tail -1 $out/$n1-steady.log)"
  done
done
