# Z の上で状態を見る小さな道具 (elf2x68k でクロスビルドし、B: などに置いて使う)
#   sst.x   : slip.sys の統計 (ETD 9) と slipd.x のカウンタ (SLPDSTAT / SLPDZSTA)
#   zprof.x : slipd.x の簡易プロファイラ (垂直同期ごとの PC) の開始・取り出し
#   ztap.x  : slipd.x が残した直近 64 パケットの先頭 (SLPDTAP_)
#   ztr.x   : slipd.x の交換のトレース (SLPDTRAC)
#   zt.x    : SCSI ID 6 の読み書き 1 回にかかる時間
#   memdump.x: メモリの一部をファイルに書く (ROM の吸い出しなど)
#   zmb.x   : SCSI のメールボックス (0xEA0060) を直に叩いて読み、IOCS と比べる
#   memsz.x : 主記憶の容量と、いま確保できる最大のブロック
#   reboot.x: IPL ROM のリセット処理へ飛んで再起動する
CC = m68k-xelf-gcc
X  = sst.x zprof.x memdump.x zmb.x ztap.x ztr.x zt.x memsz.x reboot.x

all: $(X)

sst.x: sst.c etd.S
	$(CC) -O2 -o $@ sst.c etd.S
reboot.x: reboot.S
	$(CC) -m68000 -o $@ $< -nostartfiles -nostdlib -s
%.x: %.c
	$(CC) -O2 -o $@ $<

clean:
	rm -f *.x *.elf *.o
.PHONY: all clean
