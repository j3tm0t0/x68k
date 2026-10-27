# xdfwrite.x / xdfread.x -- 2HD フロッピーと XDF イメージを IOCS で読み書きする (elf2x68k でクロスビルド)
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall

all: xdfwrite.x xdfread.x

xdfwrite.x: xdf.c
	$(CC) $(CFLAGS) -o $@ $< -s
xdfread.x: xdf.c
	$(CC) $(CFLAGS) -DXDFREAD -o $@ $< -s

clean:
	rm -f *.x *.x.elf*
.PHONY: all clean
