# modetest.x -- 画面モードの試験表示 (X68000 / X68000 Z)
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall

all: modetest.x

modetest.x: modetest.c
	$(CC) $(CFLAGS) -o $@ $< -s

clean:
	rm -f modetest.x *.x.elf*
.PHONY: all clean
