# net/ssh -- X68000 SSH クライアント (elf2x68k クロスビルド)
# libsocket.a は elf2x68k 同梱の BSD ソケット (xip.x / inetd.x 経由)。
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932
LDLIBS = -lsocket

all: tcptest.x

tcptest.x: tcptest.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f *.x *.o *.elf*
.PHONY: all clean

# 送信の試験 (xip の大きな write の不具合の確認。相手は sendsink.py)
sendtest.x: sendtest.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
