# boot/serial -- RS-232C だけで X68000 にツールを入れる道具 (rx.x は elf2x68k でクロスビルド)
CC = m68k-xelf-gcc
CFLAGS = -m68000 -Os -fomit-frame-pointer -Wall

all: rx.x

rx.x: start.S rx.c
	$(CC) $(CFLAGS) -nostartfiles -o $@ start.S rx.c -Wl,-e,_start -s

clean:
	rm -f rx.x *.elf *.o
.PHONY: all clean
