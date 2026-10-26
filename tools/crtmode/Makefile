# crtmode.x -- 画面モードを変える (X68000 / X68000 Z)
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall

all: crtmode.x

crtmode.x: crtmode.c
	$(CC) $(CFLAGS) -o $@ $< -s

clean:
	rm -f crtmode.x *.x.elf*
.PHONY: all clean
