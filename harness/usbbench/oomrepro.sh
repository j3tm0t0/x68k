#!/bin/bash
# oomrepro.sh <elf>...: 起動の窓の vfat ハンマー (UDP 6869 には何も送らない)。UART を切り出す
export PATH=$HOME/bin:$PATH; cd ~/usbbench; out=~/oomrepro; mkdir -p $out
PORT=/sys/bus/usb/devices/1-1:1.0/1-1-port3; U=/tmp/picouart-p2w.log
revive() { for i in 1 2 3; do lsusb | grep -q cafe:4002 && return 0; echo 1 | sudo tee $PORT/disable >/dev/null; sleep 2; echo 0 | sudo tee $PORT/disable >/dev/null; sleep 10; done; }
for elf in "$@"; do n=$(basename $elf .elf)
  for run in 1 2; do
    cp -p ~/pzltest.hds.delta-bak /srv/x68000z/pzltest.hds
    sudo umount /mnt/zpico 2>/dev/null
    u0=$(wc -l < $U)
    sudo python3 zfat.py --src /srv/x68000z/pzltest.hds --boot --ops 800 --seed 11 > $out/$n-$run.zfat 2>&1 & p=$!
    sleep 1
    if [ $run = 1 ]; then ok=; for i in 1 2 3; do swdflash $elf p2w > $out/$n.flash 2>&1; grep -q "Verified OK" $out/$n.flash && ! grep -q Error $out/$n.flash && { ok=1; break; }; done
      [ -z "$ok" ] && { echo "$n: flash failed"; sudo kill $p; continue 2; }
    else s=$(awk '$1=="p2w"{print $2}' ~/.picolog); (cd ~/src/openocd-rp && LD_LIBRARY_PATH=. timeout 60 ./openocd -s scripts -f interface/cmsis-dap.cfg -c "adapter serial $s" -f target/rp2350.cfg -c "gdb_port disabled" -c "init; reset; exit" >/dev/null 2>&1); fi
    for i in $(seq 40); do lsusb | grep -q cafe:4002 && break; sleep 1; done; revive
    ( sleep 300; sudo kill $p 2>/dev/null ) & k=$!; wait $p; kill $k 2>/dev/null
    sleep 3; tail -n +$((u0+1)) $U > $out/$n-$run.uart
    echo "$n run $run: $(tail -1 $out/$n-$run.zfat) | uart: $(grep -aic 'out of memory\|panic' $out/$n-$run.uart) panic, $(grep -aic 'malloc' $out/$n-$run.uart) malloc lines; usb $(lsusb | grep -c cafe:4002)"
    sudo umount /mnt/zpico 2>/dev/null; revive; sleep 5
  done
done
