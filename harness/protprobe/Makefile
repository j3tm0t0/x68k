# protprobe.x: .x を閉じ込めるのに使えるハードウェアの仕組みを調べる (elf2x68k でクロスビルド)
CC = m68k-xelf-gcc

all: protprobe.x

protprobe.x: protprobe.c trap.S
	$(CC) -m68000 -O2 -o $@ protprobe.c trap.S

test: protprobe.x
	tests/xeij_protprobe.sh

clean:
	rm -f *.x *.elf *.o
.PHONY: all test clean
