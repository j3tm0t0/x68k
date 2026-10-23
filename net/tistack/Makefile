# tis.x -- elf2x68k でクロスビルドする (C ライブラリを使わない常駐プログラム)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

all: tis.x

# start.o を先頭に置く (text の先頭が "TCP/IP" でなければならない)
tis.x: start.o tis.o lo.o pico.o main.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

%.o: %.c be.h tsp.h
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

clean:
	rm -f *.o tis.x *.elf*
.PHONY: all clean
