# net/bsrmt -- bsrmt.sys (BlueSCSI の SD カードのリモートドライブ、x68k-zos #188)。elf2x68k でクロスビルド
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932

all: bsrmt.sys

bsrmt.sys: bsrmthead.o bsrmt.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -lgcc -s

%.o: %.c x68rd_proto.h
	$(CC) $(CFLAGS) -c $<
%.o: %.S
	$(CC) -m68000 -c $<

# キャッシュと応答の確かめをホストの cc で (test/bsrmt_host_test.c)
HOSTCC ?= cc
hosttest:
	$(HOSTCC) -O1 -Wall -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-function \
	  -I test/stub -o $${TMPDIR:-/tmp}/bsrmt_host_test test/bsrmt_host_test.c && $${TMPDIR:-/tmp}/bsrmt_host_test

clean:
	rm -f *.o *.sys
.PHONY: all clean hosttest
