# npd.sys / npd.x -- elf2x68k でクロスビルドする
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

all: npd.sys npd.x

npd.sys: head.o npdsys.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s
npd.x: start.o npd.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

%.o: %.c
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

clean:
	rm -f *.o npd.sys npd.x *.elf*
.PHONY: all clean
