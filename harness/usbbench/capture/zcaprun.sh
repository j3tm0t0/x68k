#!/bin/bash
# zcaprun.sh <elf> <name> [rounds]: Z の中の sd 基板に採取用ファームを書き、電源を入れ直し、dir b:\ /w と ver を繰り返して UART ログを切り出す
export PATH=$HOME/bin:$PATH
elf=$1 name=$2 rounds=${3:-8}
L=/tmp/picouart-sd.log C=/tmp/x68con.log
n0=$(wc -l < $L); c0=$(wc -c < $C)
swdflash $elf sd
zpower cycle 5 >/dev/null
t=0; until tail -c +$c0 $C | grep -aq "^A>" ; do sleep 2; t=$((t+2)); [ $t -gt 150 ] && { echo "$name: no A> after 150 s"; break; }; done
echo "$name: A> after ${t}s"; sleep 3
for i in $(seq 1 $rounds); do
  c1=$(wc -c < $C)
  x68type 'dir b:\ /w'; sleep 15
  x68type 'ver'; sleep 3
  if tail -c +$c1 $C | grep -aq "Human68k\|Version\|version"; then echo "$name: round $i ok"; else echo "$name: round $i: no ver reply"; tail -c +$c1 $C | tr -d '\r' | tail -3; fi
done
sleep 10
tail -n +$n0 $L > ~/zcap-$name.log; tail -c +$c0 $C > ~/zcon-$name.log
echo "$name: $(wc -l < ~/zcap-$name.log) UART lines, $(grep -c 'SCSI Command' ~/zcap-$name.log) SCSI commands"
