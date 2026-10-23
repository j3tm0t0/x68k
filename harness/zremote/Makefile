# harness/zremote -- zdrv.x (リモートドライブを決まったドライブ名に)。elf2x68k でクロスビルド
CC = m68k-xelf-gcc
CFLAGS = -m68000 -O2 -Wall -finput-charset=utf-8 -fexec-charset=cp932
all: zdrv.x
zdrv.x: zdrv.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f zdrv.x zdrv.x.elf
