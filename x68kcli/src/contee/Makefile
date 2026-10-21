# contee.sys -- elf2x68k で Mac / Linux 上でクロスビルドする
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc

contee.sys: contee.o
	$(CC) -o $@ $^ -nostartfiles -nostdlib -s

%.o: %.S
	$(CC) -m68000 -finput-charset=utf-8 -fexec-charset=cp932 -c $<

clean:
	rm -f *.o contee.sys contee.sys.elf*
.PHONY: clean
