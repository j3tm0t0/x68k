# slipd.x -- elf2x68k でクロスビルド
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

slipd.x: start.o slipd.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

# ztap.x: 常駐中の slipd.x が残したパケットの先頭 (SLPDTAP_) を表示する (libc を使うので別に作る)
ztap.x: ztap.c
	$(CC) -O2 -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

clean:
	rm -f *.o slipd.x slipd.x.elf* ztap.x ztap.x.elf*
.PHONY: clean
