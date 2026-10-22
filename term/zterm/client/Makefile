# zterm の Z 側。zthost は Mac で試す版 (vt100.c のホスト版 VRAM に描く)。
# ssh.x への組み込みは net/ssh/dropbear/build.sh の ZTERM=1 (ztclient.c と ztdraw.c を vt100.c の代わりに)。
VT100   = ../../vt100
HOSTCC  = cc
SRC     = zthost.c ztclient.c ztdraw.c $(VT100)/vtdump.c
DEPS    = $(SRC) ztclient.h $(VT100)/vt100.c $(VT100)/vt100.h

zthost: $(DEPS)
	$(HOSTCC) -O2 -Wall -Wno-unused-function -DVT_HOSTTEST -I$(VT100) -o $@ $(SRC)

# 68000 版がコンパイルできるかだけ確かめる (ssh.x に入るのと同じフラグ)
CROSS   = m68k-xelf-
check68k: ztclient.c ztdraw.c ztclient.h
	$(CROSS)gcc -m68000 -O2 -fomit-frame-pointer -Wall -Wno-array-bounds -Wno-unused-function -I$(VT100) -c ztclient.c -o /dev/null
	$(CROSS)gcc -m68000 -O2 -fomit-frame-pointer -Wall -Wno-array-bounds -Wno-unused-function -I$(VT100) -c ztdraw.c -o /dev/null

clean:
	rm -f zthost
.PHONY: clean check68k
