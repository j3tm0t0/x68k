#!/bin/bash
# mac-soak.sh [秒]: p2w の USB を Mac に挿し替えて、raspi3 と同じ読み書き (全速 1200 回 + ゆっくり soak) を Mac から流す。
# raspi3 の dwc_otg (USB ホスト) と Pico の TinyUSB のどちらが ~30 秒の止まりを起こすかを分ける: Mac でも止まれば Pico 側。
# 前提: p2w の HDS0 = x68000z/pzltest.hds、macOS が /Volumes/NO NAME にマウント済み、ssh raspi3 が通る (元の HDS と UART ログ)。
# Pico の書き込み (swdflash) と UART は raspi3 の Probe のまま。結果: ./mac-soak-<時刻>/
set -u
here=$(cd "$(dirname "$0")" && pwd); dur=${1:-600}
out=$here/mac-soak-$(date +%m%d-%H%M); mkdir -p "$out"
SRC=ssh:raspi3:/srv/x68000z/pzltest.hds
u0=$(ssh raspi3 'wc -l < /tmp/picouart-p2w.log')
python3 "$here/zfat.py" --src $SRC --ops 1200 --seed 22 > "$out/vfat.log" 2>&1; echo "vfat: $(tail -1 "$out/vfat.log")"
python3 "$here/zfat.py" --src $SRC --ops 1000000 --duration "$dur" --rate 2 --seed 25 > "$out/soak.log" 2>&1; echo "soak: $(tail -1 "$out/soak.log")"
ssh raspi3 "tail -n +$((u0 + 1)) /tmp/picouart-p2w.log" > "$out/uart.log"
log show --last "$(( dur / 60 + 15 ))m" --predicate 'subsystem == "com.apple.iokit" OR eventMessage CONTAINS "USB"' --style compact 2>/dev/null | grep -i "reset\|abort\|timeout" > "$out/macos-usb.log"
grep -h "STALL\|SLOW" "$out"/vfat.log "$out"/soak.log
echo "panic lines in UART: $(grep -aic 'panic\|out of memory' "$out/uart.log"); macOS USB reset/abort lines: $(wc -l < "$out/macos-usb.log"); logs in $out"
