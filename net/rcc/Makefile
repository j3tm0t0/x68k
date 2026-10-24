# rcc.x: C のソースを rccd.py (elf2x68k の gcc) に送ってコンパイルする (libsocket。xip.x でも tis.x -p でも動く)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932
LDLIBS = -lsocket

all: rcc.x

%.x: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f rcc.x *.elf
