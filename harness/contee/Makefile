# contee.sys -- elf2x68k で Mac / Linux 上でクロスビルドする
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc

contee.sys: contee.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -s

# XVI の zmux 用: 組み込み時に 78125bps (SCC の時定数 0) にする
contee78.sys: contee.S
	$(CC) -m68000 -DTC0 -finput-charset=utf-8 -fexec-charset=cp932 -c -o contee78.o $<
	$(CC) -o $@ contee78.o -nostartfiles -nostdlib -s

%.o: %.S
	$(CC) -m68000 -finput-charset=utf-8 -fexec-charset=cp932 -c $<

clean:
	rm -f *.o contee.sys contee.sys.elf*
.PHONY: clean
