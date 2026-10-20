# slip.sys -- elf2x68k (brew install yunkya2/tap/elf2x68k) で Mac / Linux 上でクロスビルドする
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

slip.sys: head.o slip.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

%.o: %.c
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

clean:
	rm -f *.o slip.sys slip.sys.elf*
.PHONY: clean
