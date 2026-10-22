# tis.x の試験プログラム (libsocket でビルド。tis.x でも xip.x でも動く)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932
LDLIBS = -lsocket

all: tistest.x tisrun.x

%.x: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f *.x *.o *.elf*
.PHONY: all clean
