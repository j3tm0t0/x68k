# bsclock.x -- BlueSCSI v2 (Pico 2 W) が SNTP で得た時刻を X68000 の時計に設定する
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932

all: bsclock.x

bsclock.x: bsclock.c
	$(CC) $(CFLAGS) -o $@ $< -s

test: bsclock.x
	tests/xeij_test.sh

clean:
	rm -f bsclock.x *.x.elf*
.PHONY: all test clean
