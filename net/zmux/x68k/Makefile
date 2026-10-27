# net/zmux/x68k -- zmux.sys / zmux.x / zrmt.sys (elf2x68k でクロスビルド)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

all: zmux.sys zmux.x zrmt.sys idlebench.x tcset.x

zmux.sys: head.o zmux.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

zrmt.sys: zrmthead.o zrmt.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

zmux.x: start.o zmuxx.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

idlebench.x: start.o idlebench.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

tcset.x: start.o tcset.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

%.o: %.c
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

clean:
	rm -f *.o *.sys *.x *.elf
.PHONY: all clean
