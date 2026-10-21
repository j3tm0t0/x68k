# vt100 エミュレータ -- X68000 用 vtcat.x と、Mac 上でパーサを試す vthost をビルドする
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932
SRC     = vtcat.c vt100.c vtdump.c

all: vtcat.x

vtcat.x: $(SRC) vt100.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -s

# Mac 上でパーサ・スクロール・属性だけを pyte と比べる (VRAM/IOCS はスタブ)
HOSTCC  = cc
vthost: $(SRC) vt100.h
	$(HOSTCC) -O2 -Wall -DVT_HOSTTEST -o $@ $(SRC)

clean:
	rm -f vtcat.x vtcat.x.elf* vthost
.PHONY: all clean
