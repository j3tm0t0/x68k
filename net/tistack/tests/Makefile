# tis.x の試験プログラム (libsocket でビルド。tis.x でも xip.x でも動く)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932
LDLIBS = -lsocket

all: tistest.x tisrun.x

%.x: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# ホスト (Mac) で API 層とループバックを試す: make host && ./hostsim [乱数の回数]
host: hostsim.c ../tis.c ../lo.c ../be.h
	cc -O1 -Wall -Wno-unused-function -I hostinc hostsim.c ../lo.c -o hostsim

clean:
	rm -f *.x *.o *.elf* hostsim
.PHONY: all clean
